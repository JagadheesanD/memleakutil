import os
import sys
import re

def check_latest_changelog_entry():
    if not os.path.isfile('CHANGELOG.md'):
        print("CHANGELOG.md file not found!")
        sys.exit(1)

    with open('CHANGELOG.md', 'r') as file:
        lines = file.readlines()

    sample_entry_found = False
    entry_lines = []
    entry_started = False

    for line in lines:
        if line.strip() == '## Sample Entry':
            sample_entry_found = True
            continue
        
        if sample_entry_found:
            if line.startswith("## "):
                if entry_started:
                    break
                entry_started = True

            if entry_started:
                entry_lines.append(line)

    if not entry_lines:
        print("No changelog entry found after the sample entry.")
        sys.exit(1)

    latest_entry = ''.join(entry_lines).strip()

    # Regex patterns for different parts of the entry
    version_date_pattern = re.compile(r'^## \d+\.\d+\.\d+ - \d{4}-\d{2}-\d{2}$')
    change_type_pattern = re.compile(r'^### (Added|Changed|Deprecated|Fixed|Removed|Security)$')
    reason_pattern = re.compile(r'^- \*\*Reason:\*\* .{1,150}$')
    entry_end_pattern = re.compile(r'^---$')

    lines_iter = iter(latest_entry.split('\n'))
    try:
        version_date_line = next(lines_iter).strip()
        change_type_line = next(lines_iter).strip()
        reason_line = next(lines_iter).strip()
        entry_end_line = next(lines_iter).strip()
    except StopIteration:
        print("Incomplete changelog entry found. Please ensure it follows the required format.")
        sys.exit(1)

    errors = []
    if not version_date_pattern.match(version_date_line):
        errors.append("The version and date line should be in the format `## <MAJOR_VERSION_NUMBER>.<MINOR_VERSION_NUMBER>.<PATCH_VERSION_NUMBER> - <YEAR>-<MONTH>-<DATE>`.")

    if not change_type_pattern.match(change_type_line):
        errors.append("The change type line should be one of the following: `### Added`, `### Changed`, `### Deprecated`, `### Fixed`, `### Removed`, `### Security`.")

    if not reason_pattern.match(reason_line):
        errors.append("The reason line should be in the format `- **Reason:** <A reason that is 150 characters max length>`.")

    if not entry_end_pattern.match(entry_end_line):
        errors.append("The entry should end with `---`.")

    if errors:
        print("The latest CHANGELOG.md entry is not properly formatted:")
        for idx, error in enumerate(errors, 1):
            print(f"{idx}. {error}")
        print("\nLatest entry found:\n", latest_entry)
        sys.exit(1)

    print("CHANGELOG.md has been updated with a properly formatted entry.")
    sys.exit(0)

if __name__ == "__main__":
    check_latest_changelog_entry()
