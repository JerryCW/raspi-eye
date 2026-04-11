// shutdown_handler.h
// Registered reverse-order shutdown manager with per-step timeout protection.
#pragma once
#include <functional>
#include <memory>
#include <string>

class ShutdownHandler {
public:
    ShutdownHandler();
    ~ShutdownHandler();

    // No copy
    ShutdownHandler(const ShutdownHandler&) = delete;
    ShutdownHandler& operator=(const ShutdownHandler&) = delete;

    // Register a cleanup step (name + lambda).
    // Steps are executed in reverse registration order during execute().
    void register_step(const std::string& name, std::function<void()> fn);

    // Execute all registered steps in reverse order.
    // Per-step timeout: 5 seconds. Total timeout: 30 seconds.
    // Exceptions are caught and logged; execution continues.
    void execute();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
