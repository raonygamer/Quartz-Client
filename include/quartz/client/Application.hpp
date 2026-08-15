#pragma once
namespace quartz::client
{
    class Application
    {
    public:
        // Owns process-level orchestration; subsystem implementation lives behind the client module boundaries.
        int run(int argc, char* argv[]);
    };
}
