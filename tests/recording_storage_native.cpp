#include "../cpp/phonerecord.cpp"
#include <cassert>

/// Build a calendar fixture with a correctly derived day-of-week, independent of the test clock.
static SYSTEMTIME dateFixture(WORD year, WORD month, WORD day) {
    SYSTEMTIME value{}; value.wYear = year; value.wMonth = month; value.wDay = day;
    FILETIME file{}; assert(SystemTimeToFileTime(&value, &file)); assert(FileTimeToSystemTime(&file, &value)); return value;
}

/// Exercise actual production calendar expansion and validation at week, leap-day and year boundaries.
int main() {
    string folder;
    assert(recordingSubfolder("flat", "ignored", dateFixture(2026, 1, 1), folder) && folder.empty());
    assert(recordingSubfolder("daily", "", dateFixture(2024, 2, 29), folder) && folder == "2024-02-29");
    assert(recordingSubfolder("weekly", "", dateFixture(2026, 1, 1), folder) && folder == "2025-12-29");
    assert(recordingSubfolder("weekly", "", dateFixture(2026, 11, 8), folder) && folder == "2026-11-02");
    assert(recordingSubfolder("weekly", "", dateFixture(2026, 11, 9), folder) && folder == "2026-11-09");
    assert(recordingSubfolder("custom", "{yyyy}/{MM}/{dd}", dateFixture(2024, 2, 29), folder) && folder == "2024\\02\\29");
    assert(recordingSubfolder("custom", "週錄音/{weekStart}", dateFixture(2026, 1, 1), folder) && folder == "週錄音\\2025-12-29");
    assert(recordingSubfolder("custom", "Year-{yyyy}/Month-{MM}", dateFixture(2026, 11, 6), folder) && folder == "Year-2026\\Month-11");
    for (const string& invalid : {"", "../outside", "x/../../escape", "C:/outside", "/absolute", "\\\\server\\share", "{unknown}", "x//y", "x/", "CON", "nul.txt", "x/LPT1", "x/trailing.", "x/ trailing", "x/a:b", "a\nb", "{yyyy}/../{MM}"})
        assert(!recordingSubfolder("custom", invalid, dateFixture(2026, 11, 6), folder));
    Config config; config.phoneIp = config.advertiseIp = "127.0.0.1"; config.phoneUser = "fixture";
    assert(config.errors().empty());
    config.recordFolderMode = "custom"; config.recordFolderPattern = "../escape";
    const auto invalidFields = config.errors();
    assert(std::find(invalidFields.begin(), invalidFields.end(), "record_folder_pattern") != invalidFields.end());
    puts("RECORDING_STORAGE_NATIVE_OK calendars=8 invalid_patterns=17");
    return 0;
}
