"""BirdClassifier：可切换 backbone + linear classification head，可选 LoRA 微调。

frozen backbone 提取特征，仅训练 linear head 进行分类。
可选 LoRA 模式：对 Transformer backbone 的 attention 层注入低秩 adapter。
backbone 通过 BackboneConfig 注入，支持灵活切换。
"""

import torch
import torch.nn as nn

try:
    from model.training.backbone_registry import BackboneConfig
except ModuleNotFoundError:
    from training.backbone_registry import BackboneConfig


def _detect_lora_targets(model: nn.Module) -> list[str]:
    """自动检测 Transformer backbone 中 attention 层的 Q/V 投影 module 名称。

    不同 HuggingFace 模型的命名不同：
    - DINOv2 (Dinov2Model): query, value
    - DINOv3 / ViT: q_proj, v_proj
    - 通用 ViT: qkv（不拆分，不支持 LoRA）

    通过扫描模型的 named_modules 自动检测。
    """
    module_names = {name.split(".")[-1] for name, _ in model.named_modules()}

    # 按优先级检测
    candidates = [
        ["q_proj", "v_proj"],       # DINOv3, LLaMA 风格
        ["query", "value"],         # DINOv2, BERT 风格
        ["qkv"],                    # 某些 ViT 实现（合并 QKV）
    ]
    for targets in candidates:
        if all(t in module_names for t in targets):
            return targets

    raise ValueError(
        f"无法自动检测 LoRA target modules。"
        f"模型中的 module 名称: {sorted(module_names)}"
    )


class BirdClassifier(nn.Module):
    """可切换 backbone + linear classification head，可选 LoRA 微调。

    非 LoRA 模式：backbone 参数全部冻结（requires_grad=False），仅训练 linear head。
    LoRA 模式：backbone 注入 LoRA adapter，adapter 参数可训练，其余 backbone 参数冻结。
    """

    def __init__(
        self,
        num_classes: int,
        backbone_config: BackboneConfig,
        lora: bool = False,
        lora_rank: int = 8,
    ):
        """构建分类器：加载 backbone → 冻结 → 可选注入 LoRA → 创建 linear head。

        Args:
            num_classes: 分类类别数
            backbone_config: backbone 配置（来自 BACKBONE_REGISTRY）
            lora: 是否启用 LoRA 微调
            lora_rank: LoRA 低秩矩阵的 rank（仅 lora=True 时有效）
        """
        super().__init__()
        self.config = backbone_config
        self.lora_enabled = lora
        self.lora_rank = lora_rank

        # 加载 backbone 并冻结所有参数
        self.backbone = backbone_config.load_fn()
        for param in self.backbone.parameters():
            param.requires_grad = False
        self.backbone.eval()

        # 可选：注入 LoRA adapter
        lora_adapter_params = 0
        if lora:
            try:
                from peft import LoraConfig, get_peft_model
            except ImportError:
                raise ImportError(
                    "LoRA 模式需要安装 peft 库。请运行: pip install peft"
                )

            # 自动检测 attention 层的 target module 名称
            # DINOv2: query/value，DINOv3/其他: q_proj/v_proj
            target_modules = _detect_lora_targets(self.backbone)
            print(f"  LoRA target modules: {target_modules}")

            lora_config = LoraConfig(
                r=lora_rank,
                lora_alpha=lora_rank,
                target_modules=target_modules,
                lora_dropout=0.0,
                bias="none",
            )
            self.backbone = get_peft_model(self.backbone, lora_config)
            # LoRA 模式下 backbone 需要 train 模式
            self.backbone.train()
            lora_adapter_params = sum(
                p.numel()
                for p in self.backbone.parameters()
                if p.requires_grad
            )

        # 创建 linear classification head
        self.head = nn.Linear(backbone_config.feature_dim, num_classes)

        # 打印模型摘要
        frozen_params = sum(
            p.numel() for p in self.backbone.parameters() if not p.requires_grad
        )
        trainable_params = sum(p.numel() for p in self.head.parameters())
        print(f"[BirdClassifier] backbone: {backbone_config.name}")
        print(f"  frozen 参数量:    {frozen_params:,}")
        print(f"  trainable 参数量: {trainable_params:,}")
        print(f"  input_size:       {backbone_config.input_size}")
        print(f"  feature_dim:      {backbone_config.feature_dim}")
        if lora:
            print(f"  LoRA rank:        {lora_rank}")
            print(f"  LoRA adapter 参数量: {lora_adapter_params:,}")

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """前向传播：backbone 提取特征 → linear head 分类。

        LoRA 模式下 backbone 推理有梯度（LoRA 参数需要梯度流）。
        非 LoRA 模式下使用 torch.no_grad() 加速推理。

        Args:
            x: 输入图片张量，shape (B, 3, input_size, input_size)

        Returns:
            分类 logits，shape (B, num_classes)
        """
        if self.lora_enabled:
            # LoRA 模式：需要梯度流过 LoRA adapter 参数
            features = self.config.extract_fn(self.backbone, x)
        else:
            # 非 LoRA 模式：backbone 无梯度
            with torch.no_grad():
                features = self.config.extract_fn(self.backbone, x)
        return self.head(features)

    def trainable_parameters(self) -> list[nn.Parameter]:
        """返回所有可训练参数列表（传给优化器）。

        非 LoRA 模式：仅返回 head 参数。
        LoRA 模式：返回 head 参数 + LoRA adapter 参数。
        """
        if self.lora_enabled:
            # head 参数 + backbone 中 requires_grad=True 的参数（LoRA adapter）
            params = list(self.head.parameters())
            params += [
                p for p in self.backbone.parameters() if p.requires_grad
            ]
            return params
        return list(self.head.parameters())

    def merge_lora(self) -> None:
        """合并 LoRA 权重回 backbone（导出前调用）。

        调用 peft 的 merge_and_unload() 将 LoRA adapter 权重合并到原始 backbone，
        合并后模型结构与非 LoRA 模式完全一致。
        """
        if not self.lora_enabled:
            return
        self.backbone = self.backbone.merge_and_unload()
        self.lora_enabled = False
