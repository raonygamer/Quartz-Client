from pathlib import Path


def once(text: str, old: str, new: str, name: str) -> str:
    if old not in text:
        raise RuntimeError(f"{name}: anchor not found")
    return text.replace(old, new, 1)


# Make declaration parsing respect initializer nesting. Without this, a comma in
# Struct.define({ a: ..., b: ... }) could make b: look like a variable type.
p = Path("src/runtime/QuickJSSDK.cpp")
t = p.read_text()
old = '''        void stripVariableTypes(std::string& output, const std::vector<bool>& code)
        {
            static constexpr std::string_view Keywords[] = {"let", "const", "var"};
            for (std::size_t i = 0; i < output.size(); ++i)
            {
                std::size_t keywordLength = 0; for (const auto keyword : Keywords) if (wordAt(output, code, i, keyword)) { keywordLength = keyword.size(); break; }
                if (!keywordLength) continue;
                std::size_t cursor = skipSpace(output, i + keywordLength);
                while (cursor < output.size() && identifierStart(output[cursor]))
                {
                    ++cursor; while (cursor < output.size() && identifierContinue(output[cursor])) ++cursor; cursor = skipSpace(output, cursor);
                    if (cursor < output.size() && code[cursor] && output[cursor] == ':')
                    {
                        const std::size_t end = typeEnd(output, code, cursor + 1, output.size(), false); blank(output, cursor, end); cursor = end;
                    }
                    while (cursor < output.size() && (!code[cursor] || (output[cursor] != ',' && output[cursor] != ';'))) ++cursor;
                    if (cursor >= output.size() || output[cursor] == ';') break;
                    cursor = skipSpace(output, cursor + 1);
                }
            }
        }
'''
new = '''        std::size_t declarationDelimiter(const std::string& source, const std::vector<bool>& code, const std::size_t start)
        {
            int paren = 0, brace = 0, bracket = 0;
            for (std::size_t i = start; i < source.size(); ++i)
            {
                if (!code[i]) continue; const char c = source[i];
                if (c == '(') ++paren; else if (c == ')' && paren > 0) --paren;
                else if (c == '{') ++brace; else if (c == '}' && brace > 0) --brace;
                else if (c == '[') ++bracket; else if (c == ']' && bracket > 0) --bracket;
                if (!paren && !brace && !bracket && (c == ',' || c == ';')) return i;
            }
            return source.size();
        }

        void stripVariableTypes(std::string& output, const std::vector<bool>& code)
        {
            static constexpr std::string_view Keywords[] = {"let", "const", "var"};
            for (std::size_t i = 0; i < output.size(); ++i)
            {
                std::size_t keywordLength = 0; for (const auto keyword : Keywords) if (wordAt(output, code, i, keyword)) { keywordLength = keyword.size(); break; }
                if (!keywordLength) continue;
                std::size_t cursor = skipSpace(output, i + keywordLength);
                while (cursor < output.size())
                {
                    if (!identifierStart(output[cursor])) break;
                    ++cursor; while (cursor < output.size() && identifierContinue(output[cursor])) ++cursor; cursor = skipSpace(output, cursor);
                    if (cursor < output.size() && code[cursor] && output[cursor] == ':')
                    {
                        const std::size_t end = typeEnd(output, code, cursor + 1, output.size(), false); blank(output, cursor, end); cursor = skipSpace(output, end);
                    }
                    if (cursor < output.size() && code[cursor] && output[cursor] == '=') ++cursor;
                    const std::size_t delimiter = declarationDelimiter(output, code, cursor);
                    if (delimiter >= output.size() || output[delimiter] == ';') break;
                    cursor = skipSpace(output, delimiter + 1);
                }
            }
        }
'''
t = once(t, old, new, "nest-aware variable annotations")

# Pointer metadata uses reserved $ names so real remote fields called address,
# process, etc. never collide with the Pointer wrapper itself.
t = t.replace('const pointer = value && typeof value === "object" && "address" in value ? value.address : value;', 'const pointer = value && typeof value === "object" && "$address" in value ? value.$address : value;')
t = t.replace('''    const target = {
        process,
        address: address(targetAddress),
        get readable() { return this.isReadable(); },
        isReadable() { try { api.memory.read(process.pid, this.address, "u8"); return true; } catch { return false; } },
        as(nextType) { return pointerFor(nextType, process, this.address); }
    };''', '''    const target = {
        $process: process,
        $address: address(targetAddress),
        get $readable() { return this.isReadable(); },
        isReadable() { try { api.memory.read(process.pid, this.$address, "u8"); return true; } catch { return false; } },
        as(nextType) { return pointerFor(nextType, process, this.$address); }
    };''')
t = t.replace('return field ? readField(process, object.address, field) : undefined;', 'return field ? readField(process, object.$address, field) : undefined;')
t = t.replace('writeField(process, object.address, field, value); return true;', 'writeField(process, object.$address, field, value); return true;')
if 'object.address' in t or '"address" in value ? value.address' in t:
    raise RuntimeError("Pointer metadata address collision survived")
p.write_text(t)


p = Path("src/runtime/QuickJSTypes.cpp")
t = p.read_text()
t = once(t, '''    export interface PointerBase {
        readonly process: Process;
        readonly address: Address;
        readonly readable: boolean;
        isReadable(): boolean;
        as<TFields extends StructFields>(type: Struct<TFields>): Pointer<Struct<TFields>>;
    }
''', '''    export interface PointerBase {
        /** Process/address metadata use reserved $ names so struct fields can freely use names such as address or process. */
        readonly $process: Process;
        readonly $address: Address;
        readonly $readable: boolean;
        isReadable(): boolean;
        as<TFields extends StructFields>(type: Struct<TFields>): Pointer<Struct<TFields>>;
    }
''', "PointerBase declarations")
p.write_text(t)

print("TypeScript runtime correctness fixups applied")
