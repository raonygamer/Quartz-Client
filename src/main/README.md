# Main.cpp structural split

These `.inc` fragments intentionally remain one translation unit. This containment pass preserves anonymous-namespace linkage, declaration order, static initialization and destruction order while making the former 700+ KiB `Main.cpp` navigable.

The splitter round-trips the original namespace body byte-for-byte and only cuts at lexer-safe top-level boundaries. `MainEntry.inc` contains `main()` outside the anonymous namespace.

## Include order

1. `MainShaderSources.inc` (43562 bytes)
2. `MainShaderSources02.inc` (47843 bytes)
3. `MainInput.inc` (44166 bytes)
4. `MainInput02.inc` (42938 bytes)
5. `MainMedia.inc` (51416 bytes)
6. `MainRuntimeModel.inc` (42084 bytes)
7. `MainRuntimeNative.inc` (45744 bytes)
8. `MainRuntimeModel02.inc` (155989 bytes)
9. `MainRuntimeBindings.inc` (42087 bytes)
10. `MainRuntimeControls.inc` (49484 bytes)
11. `MainRuntimeModel03.inc` (43032 bytes)
12. `MainRuntimeBindings02.inc` (42447 bytes)
13. `MainUI.inc` (54296 bytes)
14. `MainEntry.inc` (21152 bytes)
