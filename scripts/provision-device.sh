#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# scripts/provision-device.sh
# AWS IoT device provisioning script
# Supports: provision (default) | verify | cleanup
# ============================================================

# ============================================================
# Global variables (from args, never hardcoded)
# ============================================================
THING_NAME=""
PROJECT_NAME="RaspiEye"
OUTPUT_DIR="device/certs"
POLICY_NAME=""
ROLE_NAME=""
ROLE_ALIAS=""
MODE="provision"  # provision | verify | cleanup

# Runtime variables (dynamically resolved)
AWS_ACCOUNT_ID=""
AWS_REGION=""
CERT_ARN=""
CERT_ID=""
CREDENTIAL_ENDPOINT=""

# ============================================================
# Logging utilities
# ============================================================

log_info() {
    printf "[INFO] %s\n" "$1"
}

log_warn() {
    printf "[WARN] %s\n" "$1"
}

log_error() {
    printf "[ERROR] %s\n" "$1" >&2
}

log_success() {
    printf "[OK] %s\n" "$1"
}

# ============================================================
# Dependency and credential checks
# ============================================================

check_dependencies() {
    local missing=()

    for cmd in aws jq curl; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Missing required tools: ${missing[*]}"
        log_error "Please install them before running this script"
        exit 1
    fi

    log_info "Verifying AWS credentials..."
    if ! aws sts get-caller-identity &>/dev/null; then
        log_error "AWS credentials not configured or invalid"
        log_error "Run 'aws configure' or set AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY"
        exit 1
    fi

    log_success "All dependencies and credentials verified"
}

# ============================================================
# AWS context resolution
# ============================================================

get_aws_context() {
    AWS_ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)

    if [[ -z "$AWS_REGION" ]]; then
        AWS_REGION=$(aws configure get region || echo "")
    fi

    if [[ -z "$AWS_ACCOUNT_ID" ]]; then
        log_error "Failed to retrieve AWS Account ID"
        exit 1
    fi

    if [[ -z "$AWS_REGION" ]]; then
        log_error "AWS Region not configured. Use --region or run 'aws configure' to set a default region"
        exit 1
    fi

    export AWS_DEFAULT_REGION="$AWS_REGION"
    log_info "AWS Account: ${AWS_ACCOUNT_ID}, Region: ${AWS_REGION}"
}

# ============================================================
# Usage / help
# ============================================================

print_usage() {
    local script_name
    script_name="$(basename "$0")"
    printf "Usage: %s [OPTIONS]\n" "$script_name"
    printf "\n"
    printf "AWS IoT device provisioning script.\n"
    printf "Creates IoT Thing, certificates, policy, IAM role, and role alias.\n"
    printf "\n"
    printf "Required:\n"
    printf "  --thing-name NAME        IoT Thing name (e.g. RaspiEyeAlpha)\n"
    printf "\n"
    printf "Optional:\n"
    printf "  --project-name NAME      Project name for shared resources (default: RaspiEye)\n"
    printf "  --output-dir DIR         Certificate output directory (default: device/certs)\n"
    printf "  --policy-name NAME       IoT Policy name (default: {project-name}IotPolicy)\n"
    printf "  --role-name NAME         IAM Role name (default: {project-name}IotRole)\n"
    printf "  --role-alias NAME        Role Alias name (default: {project-name}RoleAlias)\n"
    printf "  --region REGION          AWS Region (default: from aws configure)\n"
    printf "  --verify                 Verify mode: check existing resources\n"
    printf "  --cleanup                Cleanup mode: delete all resources\n"
    printf "  --help                   Show this help message\n"
    printf "\n"
    printf "Examples:\n"
    printf "  %s --thing-name RaspiEyeAlpha --region ap-northeast-1\n" "$script_name"
    printf "  %s --thing-name RaspiEyeAlpha --verify\n" "$script_name"
    printf "  %s --thing-name RaspiEyeAlpha --cleanup\n" "$script_name"
}

# ============================================================
# Argument parsing
# ============================================================

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --thing-name)
                THING_NAME="$2"
                shift 2
                ;;
            --output-dir)
                OUTPUT_DIR="$2"
                shift 2
                ;;
            --project-name)
                PROJECT_NAME="$2"
                shift 2
                ;;
            --policy-name)
                POLICY_NAME="$2"
                shift 2
                ;;
            --role-name)
                ROLE_NAME="$2"
                shift 2
                ;;
            --role-alias)
                ROLE_ALIAS="$2"
                shift 2
                ;;
            --region)
                AWS_REGION="$2"
                shift 2
                ;;
            --verify)
                MODE="verify"
                shift
                ;;
            --cleanup)
                MODE="cleanup"
                shift
                ;;
            --help)
                print_usage
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                print_usage
                exit 1
                ;;
        esac
    done

    # Validate required args
    if [[ -z "$THING_NAME" ]]; then
        log_error "Missing required argument: --thing-name"
        print_usage
        exit 1
    fi

    # Derive defaults from project name (shared resources, PascalCase)
    POLICY_NAME="${POLICY_NAME:-${PROJECT_NAME}IotPolicy}"
    ROLE_NAME="${ROLE_NAME:-${PROJECT_NAME}IotRole}"
    ROLE_ALIAS="${ROLE_ALIAS:-${PROJECT_NAME}RoleAlias}"

    log_info "Thing: ${THING_NAME}, Mode: ${MODE}"
    log_info "Policy: ${POLICY_NAME}, Role: ${ROLE_NAME}, Alias: ${ROLE_ALIAS}"
    log_info "Output dir: ${OUTPUT_DIR}"
}

# ============================================================
# Provision functions — IoT Thing & Certificate (Task 3)
# ============================================================

create_thing() {
    log_info "Checking IoT Thing '${THING_NAME}'..."
    if aws iot describe-thing --thing-name "$THING_NAME" &>/dev/null; then
        log_info "Thing '${THING_NAME}' already exists, skipping"
        return 0
    fi

    log_info "Creating IoT Thing '${THING_NAME}'..."
    local result
    result=$(aws iot create-thing --thing-name "$THING_NAME")
    local thing_arn
    thing_arn=$(printf '%s' "$result" | jq -r '.thingArn')
    log_success "Created Thing: ${thing_arn}"
}

create_certificate() {
    local cert_file="${OUTPUT_DIR}/device-cert.pem"
    local key_file="${OUTPUT_DIR}/device-private.key"

    if [[ -f "$cert_file" ]] && [[ -f "$key_file" ]]; then
        log_info "Certificate files already exist in '${OUTPUT_DIR}', skipping creation"
        return 0
    fi

    mkdir -p "$OUTPUT_DIR"

    log_info "Creating keys and certificate..."
    local result
    result=$(aws iot create-keys-and-certificate --set-as-active)

    CERT_ARN=$(printf '%s' "$result" | jq -r '.certificateArn')
    CERT_ID=$(printf '%s' "$result" | jq -r '.certificateId')

    local cert_pem
    cert_pem=$(printf '%s' "$result" | jq -r '.certificatePem')
    local private_key
    private_key=$(printf '%s' "$result" | jq -r '.keyPair.PrivateKey')

    printf '%s\n' "$cert_pem" > "$cert_file"
    printf '%s\n' "$private_key" > "$key_file"

    chmod 600 "$cert_file"
    chmod 600 "$key_file"

    log_success "Certificate created: ID=${CERT_ID}"
    log_success "Certificate ARN: ${CERT_ARN}"
    log_info "Saved certificate to ${cert_file}"
    log_info "Saved private key to ${key_file}"
}

download_root_ca() {
    local ca_file="${OUTPUT_DIR}/root-ca.pem"

    if [[ -f "$ca_file" ]]; then
        log_info "Root CA already exists at '${ca_file}', skipping download"
        return 0
    fi

    mkdir -p "$OUTPUT_DIR"

    log_info "Downloading Amazon Root CA 1..."
    curl -s -o "$ca_file" https://www.amazontrust.com/repository/AmazonRootCA1.pem
    log_success "Downloaded Root CA to ${ca_file}"
}

attach_cert_to_thing() {
    log_info "Checking if certificate is attached to Thing '${THING_NAME}'..."
    local principals
    principals=$(aws iot list-thing-principals --thing-name "$THING_NAME" --query 'principals' --output text 2>/dev/null || echo "")

    if printf '%s' "$principals" | grep -q "$CERT_ARN"; then
        log_info "Certificate already attached to Thing '${THING_NAME}', skipping"
        return 0
    fi

    log_info "Attaching certificate to Thing '${THING_NAME}'..."
    aws iot attach-thing-principal --thing-name "$THING_NAME" --principal "$CERT_ARN"
    log_success "Certificate attached to Thing '${THING_NAME}'"
}

recover_cert_arn() {
    local cert_file="${OUTPUT_DIR}/device-cert.pem"

    # Only recover if local cert exists but CERT_ARN is empty
    if [[ ! -f "$cert_file" ]]; then
        return 0
    fi

    if [[ -n "$CERT_ARN" ]]; then
        return 0
    fi

    log_info "Local certificate exists but CERT_ARN unknown, recovering from AWS..."
    local principal
    principal=$(aws iot list-thing-principals \
        --thing-name "$THING_NAME" \
        --query 'principals[0]' \
        --output text 2>/dev/null || echo "")

    if [[ -z "$principal" ]] || [[ "$principal" == "None" ]]; then
        log_error "Local cert files exist but no certificate found in AWS for thing '${THING_NAME}'"
        log_error "Run with --cleanup first, then re-provision"
        exit 1
    fi

    CERT_ARN="$principal"
    # Extract certificate ID from ARN (last segment after '/')
    CERT_ID=$(printf '%s' "$CERT_ARN" | awk -F'/' '{print $NF}')
    log_success "Recovered CERT_ARN: ${CERT_ARN}"
    log_success "Recovered CERT_ID: ${CERT_ID}"
}

# ============================================================
# Provision functions — Policy, Role, Role Alias (Task 4)
# ============================================================

create_iot_policy() {
    log_info "Checking IoT Policy '${POLICY_NAME}'..."
    if aws iot get-policy --policy-name "$POLICY_NAME" &>/dev/null; then
        log_info "Policy '${POLICY_NAME}' already exists, skipping"
        return 0
    fi

    log_info "Creating IoT Policy '${POLICY_NAME}'..."
    local policy_doc
    policy_doc=$(printf '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"iot:Connect","Resource":"arn:aws:iot:%s:%s:client/${iot:Connection.Thing.ThingName}"},{"Effect":"Allow","Action":"iot:AssumeRoleWithCertificate","Resource":"arn:aws:iot:%s:%s:rolealias/%s"}]}' \
        "$AWS_REGION" "$AWS_ACCOUNT_ID" "$AWS_REGION" "$AWS_ACCOUNT_ID" "$ROLE_ALIAS")

    aws iot create-policy \
        --policy-name "$POLICY_NAME" \
        --policy-document "$policy_doc" > /dev/null
    log_success "Created IoT Policy '${POLICY_NAME}'"
}

attach_policy_to_cert() {
    log_info "Checking if Policy '${POLICY_NAME}' is attached to certificate..."
    local attached
    attached=$(aws iot list-attached-policies --target "$CERT_ARN" --query 'policies[].policyName' --output text 2>/dev/null || echo "")

    if printf '%s' "$attached" | grep -q "$POLICY_NAME"; then
        log_info "Policy '${POLICY_NAME}' already attached to certificate, skipping"
        return 0
    fi

    log_info "Attaching Policy '${POLICY_NAME}' to certificate..."
    aws iot attach-policy --policy-name "$POLICY_NAME" --target "$CERT_ARN"
    log_success "Policy '${POLICY_NAME}' attached to certificate"
}

create_iam_role() {
    log_info "Checking IAM Role '${ROLE_NAME}'..."
    if aws iam get-role --role-name "$ROLE_NAME" &>/dev/null; then
        log_info "IAM Role '${ROLE_NAME}' already exists, skipping"
        return 0
    fi

    log_info "Creating IAM Role '${ROLE_NAME}'..."
    local trust_policy
    trust_policy=$(printf '{"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"Service":"credentials.iot.amazonaws.com"},"Action":"sts:AssumeRole"}]}')

    aws iam create-role \
        --role-name "$ROLE_NAME" \
        --assume-role-policy-document "$trust_policy" > /dev/null
    log_success "Created IAM Role '${ROLE_NAME}'"
}

create_role_alias() {
    log_info "Checking Role Alias '${ROLE_ALIAS}'..."
    if aws iot describe-role-alias --role-alias "$ROLE_ALIAS" &>/dev/null; then
        log_info "Role Alias '${ROLE_ALIAS}' already exists, skipping"
        return 0
    fi

    local role_arn
    role_arn=$(aws iam get-role --role-name "$ROLE_NAME" --query 'Role.Arn' --output text)

    log_info "Creating Role Alias '${ROLE_ALIAS}'..."
    aws iot create-role-alias \
        --role-alias "$ROLE_ALIAS" \
        --role-arn "$role_arn" > /dev/null
    log_success "Created Role Alias '${ROLE_ALIAS}'"
}

get_credential_endpoint() {
    log_info "Retrieving IoT Credential Provider endpoint..."
    CREDENTIAL_ENDPOINT=$(aws iot describe-endpoint \
        --endpoint-type iot:CredentialProvider \
        --query endpointAddress \
        --output text)
    log_success "Credential endpoint: ${CREDENTIAL_ENDPOINT}"
}

# ============================================================
# Provision functions — TOML config & summary (Task 5)
# ============================================================

generate_toml_config() {
    local config_dir="device/config"
    local config_file="${config_dir}/config.toml"
    mkdir -p "$config_dir"

    local aws_section=""
    aws_section+="[aws]"
    aws_section+=$'\n'
    aws_section+="thing_name = \"${THING_NAME}\""
    aws_section+=$'\n'
    aws_section+="credential_endpoint = \"${CREDENTIAL_ENDPOINT}\""
    aws_section+=$'\n'
    aws_section+="role_alias = \"${ROLE_ALIAS}\""
    aws_section+=$'\n'
    aws_section+="cert_path = \"${OUTPUT_DIR}/device-cert.pem\""
    aws_section+=$'\n'
    aws_section+="key_path = \"${OUTPUT_DIR}/device-private.key\""
    aws_section+=$'\n'
    aws_section+="ca_path = \"${OUTPUT_DIR}/root-ca.pem\""

    if [[ -f "$config_file" ]]; then
        log_info "Updating [aws] section in existing ${config_file}..."
        local tmp_file="${config_file}.tmp"
        awk '/^\[aws\]/{skip=1; next} /^\[/{skip=0} !skip' "$config_file" > "$tmp_file"
        printf '\n%s\n' "$aws_section" >> "$tmp_file"
        mv "$tmp_file" "$config_file"
    else
        log_info "Creating ${config_file}..."
        printf '%s\n' "$aws_section" > "$config_file"
    fi

    log_success "TOML config written to ${config_file}"
}

print_summary() {
    local role_arn
    role_arn=$(aws iam get-role --role-name "$ROLE_NAME" --query 'Role.Arn' --output text 2>/dev/null || echo "N/A")

    printf "\n"
    log_info "========== Provision Summary =========="
    log_info "Thing Name:          ${THING_NAME}"
    log_info "Certificate ARN:     ${CERT_ARN}"
    log_info "Certificate ID:      ${CERT_ID}"
    log_info "Policy Name:         ${POLICY_NAME}"
    log_info "IAM Role ARN:        ${role_arn}"
    log_info "Role Alias:          ${ROLE_ALIAS}"
    log_info "Credential Endpoint: ${CREDENTIAL_ENDPOINT}"
    log_info "Cert file:           ${OUTPUT_DIR}/device-cert.pem"
    log_info "Key file:            ${OUTPUT_DIR}/device-private.key"
    log_info "CA file:             ${OUTPUT_DIR}/root-ca.pem"
    log_info "Config file:         device/config/config.toml"
    log_info "======================================="
    log_success "Provisioning complete!"
}

do_provision() {
    create_thing
    create_certificate
    download_root_ca
    recover_cert_arn
    attach_cert_to_thing
    create_iot_policy
    attach_policy_to_cert
    create_iam_role
    create_role_alias
    get_credential_endpoint
    generate_toml_config
    print_summary
}

verify_resources() {
    log_info "Verifying resources for Thing '${THING_NAME}'..."
    local failures=0

    # 1. Thing
    if aws iot describe-thing --thing-name "$THING_NAME" &>/dev/null; then
        log_success "Thing '${THING_NAME}' exists"
    else
        log_error "[FAIL] Thing '${THING_NAME}' not found"
        failures=$((failures + 1))
    fi

    # 2. Certificate files
    local cert_file="${OUTPUT_DIR}/device-cert.pem"
    local key_file="${OUTPUT_DIR}/device-private.key"
    local ca_file="${OUTPUT_DIR}/root-ca.pem"

    if [[ -f "$cert_file" ]]; then
        log_success "Certificate file: ${cert_file}"
    else
        log_error "[FAIL] Certificate file missing: ${cert_file}"
        failures=$((failures + 1))
    fi

    if [[ -f "$key_file" ]]; then
        log_success "Private key file: ${key_file}"
    else
        log_error "[FAIL] Private key file missing: ${key_file}"
        failures=$((failures + 1))
    fi

    if [[ -f "$ca_file" ]]; then
        log_success "Root CA file: ${ca_file}"
    else
        log_error "[FAIL] Root CA file missing: ${ca_file}"
        failures=$((failures + 1))
    fi

    # 3. Policy
    if aws iot get-policy --policy-name "$POLICY_NAME" &>/dev/null; then
        log_success "Policy '${POLICY_NAME}' exists"
    else
        log_error "[FAIL] Policy '${POLICY_NAME}' not found"
        failures=$((failures + 1))
    fi

    # 4. IAM Role
    if aws iam get-role --role-name "$ROLE_NAME" &>/dev/null; then
        log_success "IAM Role '${ROLE_NAME}' exists"
    else
        log_error "[FAIL] IAM Role '${ROLE_NAME}' not found"
        failures=$((failures + 1))
    fi

    # 5. Role Alias
    if aws iot describe-role-alias --role-alias "$ROLE_ALIAS" &>/dev/null; then
        log_success "Role Alias '${ROLE_ALIAS}' exists"
    else
        log_error "[FAIL] Role Alias '${ROLE_ALIAS}' not found"
        failures=$((failures + 1))
    fi

    # 6. TOML config file
    local config_file="device/config/config.toml"
    if [[ -f "$config_file" ]]; then
        log_success "TOML config: ${config_file}"
    else
        log_error "[FAIL] TOML config missing: ${config_file}"
        failures=$((failures + 1))
    fi

    # Summary
    printf "\n"
    if [[ $failures -eq 0 ]]; then
        log_success "All resources verified successfully"
    else
        log_error "${failures} resource(s) failed verification"
    fi
}

cleanup_resources() {
    log_info "Cleaning up resources for Thing '${THING_NAME}'..."
    local errors=0

    # Step 0: Retrieve CERT_ARN from AWS (needed for detach/delete steps)
    local CERT_ARN_CLEANUP=""
    CERT_ARN_CLEANUP=$(aws iot list-thing-principals \
        --thing-name "$THING_NAME" \
        --query 'principals[0]' \
        --output text 2>/dev/null || echo "")

    local CERT_ID_CLEANUP=""
    if [[ -n "$CERT_ARN_CLEANUP" ]] && [[ "$CERT_ARN_CLEANUP" != "None" ]]; then
        CERT_ID_CLEANUP=$(printf '%s' "$CERT_ARN_CLEANUP" | awk -F'/' '{print $NF}')
        log_info "Found certificate: ${CERT_ID_CLEANUP}"
    else
        log_warn "No certificate found for Thing '${THING_NAME}', skipping certificate-related steps"
    fi

    # Step 1: Detach certificate from Thing
    if [[ -n "$CERT_ARN_CLEANUP" ]] && [[ "$CERT_ARN_CLEANUP" != "None" ]]; then
        log_info "Detaching certificate from Thing '${THING_NAME}'..."
        aws iot detach-thing-principal \
            --thing-name "$THING_NAME" \
            --principal "$CERT_ARN_CLEANUP" 2>/dev/null \
            || { log_warn "Failed to detach certificate from Thing"; errors=$((errors + 1)); }
    fi

    # Step 2: Detach Policy from certificate
    if [[ -n "$CERT_ARN_CLEANUP" ]] && [[ "$CERT_ARN_CLEANUP" != "None" ]]; then
        log_info "Detaching Policy '${POLICY_NAME}' from certificate..."
        aws iot detach-policy \
            --policy-name "$POLICY_NAME" \
            --target "$CERT_ARN_CLEANUP" 2>/dev/null \
            || { log_warn "Failed to detach Policy from certificate"; errors=$((errors + 1)); }
    fi

    # Step 3: Deactivate + delete certificate
    if [[ -n "$CERT_ID_CLEANUP" ]]; then
        log_info "Deactivating certificate '${CERT_ID_CLEANUP}'..."
        aws iot update-certificate \
            --certificate-id "$CERT_ID_CLEANUP" \
            --new-status INACTIVE 2>/dev/null \
            || { log_warn "Failed to deactivate certificate"; errors=$((errors + 1)); }

        log_info "Deleting certificate '${CERT_ID_CLEANUP}'..."
        aws iot delete-certificate \
            --certificate-id "$CERT_ID_CLEANUP" 2>/dev/null \
            || { log_warn "Failed to delete certificate"; errors=$((errors + 1)); }
    fi

    # Step 4: Delete IoT Policy
    log_info "Deleting IoT Policy '${POLICY_NAME}'..."
    aws iot delete-policy \
        --policy-name "$POLICY_NAME" 2>/dev/null \
        || { log_warn "Failed to delete IoT Policy"; errors=$((errors + 1)); }

    # Step 5: Delete Role Alias
    log_info "Deleting Role Alias '${ROLE_ALIAS}'..."
    aws iot delete-role-alias \
        --role-alias "$ROLE_ALIAS" 2>/dev/null \
        || { log_warn "Failed to delete Role Alias"; errors=$((errors + 1)); }

    # Step 6: Delete IAM Role
    log_info "Deleting IAM Role '${ROLE_NAME}'..."
    aws iam delete-role \
        --role-name "$ROLE_NAME" 2>/dev/null \
        || { log_warn "Failed to delete IAM Role"; errors=$((errors + 1)); }

    # Step 7: Delete Thing
    log_info "Deleting IoT Thing '${THING_NAME}'..."
    aws iot delete-thing \
        --thing-name "$THING_NAME" 2>/dev/null \
        || { log_warn "Failed to delete IoT Thing"; errors=$((errors + 1)); }

    # Step 8: Delete local certificate files and TOML config
    log_info "Deleting local certificate files and config..."
    rm -f "${OUTPUT_DIR}/device-cert.pem" 2>/dev/null \
        || { log_warn "Failed to delete device-cert.pem"; errors=$((errors + 1)); }
    rm -f "${OUTPUT_DIR}/device-private.key" 2>/dev/null \
        || { log_warn "Failed to delete device-private.key"; errors=$((errors + 1)); }
    rm -f "${OUTPUT_DIR}/root-ca.pem" 2>/dev/null \
        || { log_warn "Failed to delete root-ca.pem"; errors=$((errors + 1)); }
    rm -f "device/config/config.toml" 2>/dev/null \
        || { log_warn "Failed to delete config.toml"; errors=$((errors + 1)); }

    # Summary
    printf "\n"
    if [[ $errors -eq 0 ]]; then
        log_success "All resources cleaned up successfully"
    else
        log_warn "Cleanup completed with ${errors} warning(s)"
    fi
}

# ============================================================
# Main entry point
# ============================================================

main() {
    parse_args "$@"
    check_dependencies
    get_aws_context

    case "$MODE" in
        provision)
            do_provision
            ;;
        verify)
            verify_resources
            ;;
        cleanup)
            cleanup_resources
            ;;
    esac
}

main "$@"
