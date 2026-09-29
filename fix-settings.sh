#!/bin/bash
# Re-adds the Rubber Band build settings that projectGenerator discards.
set -e
sed -i '' 's|HEADER_SEARCH_PATHS = (|HEADER_SEARCH_PATHS = (\n\t\t\t\t\t"/opt/homebrew/include",|g' vocalyx.xcodeproj/project.pbxproj
sed -i '' 's|OTHER_LDFLAGS = (|OTHER_LDFLAGS = (\n\t\t\t\t\t"-lrubberband",|g' vocalyx.xcodeproj/project.pbxproj
echo "settings patched"
