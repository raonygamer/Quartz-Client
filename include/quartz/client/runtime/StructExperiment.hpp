#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::client
{
    enum class StructExperimentFieldKind : std::uint8_t
    {
        I8, U8, I16, U16, I32, U32, I64, U64, F32, F64, Bool, Pointer, Struct, Array
    };

    struct StructExperimentDefinition;
    struct StructExperimentField
    {
        std::string Name;
        StructExperimentFieldKind Kind = StructExperimentFieldKind::I32;
        std::uintptr_t Offset = 0;
        std::size_t Size = 0;
        std::size_t Count = 0;
        std::shared_ptr<StructExperimentDefinition> Nested;
        std::shared_ptr<StructExperimentField> Element;
    };

    struct StructExperimentDefinition { std::vector<StructExperimentField> Fields; };

    bool runtimeEvaluateStructExperiment(std::string_view source, StructExperimentDefinition& definition, std::string& error);
    const char* runtimeStructExperimentFieldKindName(StructExperimentFieldKind kind) noexcept;
}
