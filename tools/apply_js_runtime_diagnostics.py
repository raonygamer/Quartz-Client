from pathlib import Path

path = Path(__file__).resolve().parents[1] / "src/ui/pages/JavaScriptPage.cpp"
text = path.read_text()
old = 'static std::unordered_map<std::uint64_t, bool> autoScroll; bool& follow = autoScroll[script.Id]; if (!autoScroll.contains(script.Id)) follow = true;'
new = 'static std::unordered_map<std::uint64_t, bool> autoScroll; auto [followIt, inserted] = autoScroll.try_emplace(script.Id, true); bool& follow = followIt->second;'
if old in text:
    path.write_text(text.replace(old, new, 1))
elif new not in text:
    raise RuntimeError("console auto-scroll source fragment not found")
print("JavaScript console auto-scroll default fixed")
