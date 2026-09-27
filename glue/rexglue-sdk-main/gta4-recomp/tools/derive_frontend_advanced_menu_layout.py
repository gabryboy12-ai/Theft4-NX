#!/usr/bin/env python3

from pathlib import Path
import re


RETAIL_SLOTS = 20
OPTION_RECORD_BYTES = 22
OBSERVED_STOCK_ROWS = 12


def page_slices(setting_count: int) -> list[tuple[int, int, bool, bool]]:
    result = []
    first = 0
    while first < setting_count:
        has_previous = first != 0
        remaining = setting_count - first
        last_page_capacity = RETAIL_SLOTS - 3 - int(has_previous)
        has_next = remaining > last_page_capacity
        item_capacity = last_page_capacity - int(has_next)
        item_count = min(remaining, item_capacity)
        result.append((first, item_count, has_previous, has_next))
        first += item_count
    return result


def main() -> None:
    generated = Path(__file__).parents[1] / "generated" / "gta4_recomp.11.cpp"
    source = generated.read_text(encoding="utf-8")
    assert "cmpwi cr6,r7,20" in source
    assert "li r3,3232" in source

    generated_text = Path(__file__).parents[1] / "generated" / "gta4_recomp.7.cpp"
    text_source = generated_text.read_text(encoding="utf-8")
    assert "DEFINE_REX_FUNC(sub_8221FD88)" in text_source
    assert "sub_8221F840(ctx, base);" in text_source

    hooks = Path(__file__).parents[1] / "src" / "gta4_frontend_hooks.cpp"
    hook_source = hooks.read_text(encoding="utf-8")
    settings_source = hook_source[
        hook_source.index("constexpr std::array kSettings") : hook_source.index(
            "constexpr std::string_view kAdvancedKey"
        )
    ]
    setting_keys = re.findall(r'Setting\{"([^"]+)"', settings_source)
    assert max(len(key.encode("utf-8")) for key in setting_keys) < 16
    string_pool_source = hook_source[
        hook_source.index("constexpr std::array<std::string_view") : hook_source.index(
            "struct NativePageState"
        )
    ]
    texts = re.findall(r'^\s+"([^"]*)",$', string_pool_source, re.MULTILINE)
    assert texts
    slices = page_slices(len(setting_keys))
    native_page_rows = [
        item_count + 3 + int(has_previous) + int(has_next)
        for _, item_count, has_previous, has_next in slices
    ]
    primary_rows = OBSERVED_STOCK_ROWS + 1
    primary_row_bytes = primary_rows * OPTION_RECORD_BYTES
    native_row_bytes = sum(native_page_rows) * OPTION_RECORD_BYTES
    string_pool_bytes = sum(len(text.encode("utf-8")) + 1 for text in texts)
    allocation_bytes = primary_row_bytes + native_row_bytes + string_pool_bytes

    assert primary_rows <= RETAIL_SLOTS
    assert all(row_count <= RETAIL_SLOTS for row_count in native_page_rows)
    assert max(
        len(key)
        for key in ("LR_ADVANCED", "LR_SAVE", "LR_BACK", "LR_PREV", "LR_NEXT")
    ) < 16
    assert "InstallDisplayExtension" in hook_source
    assert "ReleaseDisplayExtension(ctx, base);" in hook_source
    assert "ctx.r3.u64 = text_address;" in hook_source
    assert "g_string_pool_address" not in hook_source
    assert "InvokeGuest(ctx, base, sub_821B3560, old_rows)" not in hook_source

    print(f"primary_rows={primary_rows}")
    print(f"native_pages={len(slices)}")
    print(f"native_page_rows={native_page_rows}")
    print(f"primary_row_bytes={primary_row_bytes}")
    print(f"native_row_bytes={native_row_bytes}")
    print(f"string_pool_bytes={string_pool_bytes}")
    print(f"allocation_bytes={allocation_bytes}")
    print("retail_generated_contract=verified")


if __name__ == "__main__":
    main()
