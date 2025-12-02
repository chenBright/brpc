#!/bin/bash

if [ $# -ne 1 ]; then
    echo 'Usage: $0 <new_version>'
    echo 'Example: $0 21.7'
    exit 1
fi

NEW_VERSION="$1"
MODULE_FILE='MODULE.bazel'

if [ ! -f "$MODULE_FILE" ]; then
    echo "Error: $MODULE_FILE not found"
    exit 1
fi

TEMP_FILE=$(mktemp)

# Scan file to find relevant lines.
bazel_dep_line_num=0
override_exists=false
current_line_num=0

echo "Scanning $MODULE_FILE..."

while read -r line; do
    current_line_num=$((current_line_num + 1))

    # Skip comment lines.
    if [[ "$line" =~ ^[[:space:]]*# ]]; then
        continue
    fi

    # Check if this is protobuf bazel_dep line.
    if [[ "$line" =~ bazel_dep.*name[[:space:]]*=[[:space:]]*[\'\"]*protobuf[\'\"]*.*version ]]; then
        bazel_dep_line_num=$current_line_num
        echo "Found bazel_dep for protobuf at line $current_line_num"
    fi

    # Check if protobuf single_version_override exists.
    if [[ "$line" =~ single_version_override.*module_name[[:space:]]*=[[:space:]]*[\'\"]*protobuf[\'\"]*.*version ]]; then
        override_exists=true
        echo "Found existing single_version_override for protobuf at line $current_line_num"
    fi
done < "$MODULE_FILE"

# If bazel_dep protobuf line not found, returns error.
if [ "$bazel_dep_line_num" -eq 0 ]; then
    echo 'Error: bazel_dep for protobuf not found in MODULE.bazel'
    exit 1
fi

# Process file.
echo "Processing $MODULE_FILE..."
current_line_num=0
while read -r line; do
    current_line_num=$((current_line_num + 1))

    # If this is single_version_override protobuf line and not a comment, replace version
    if [[ "$line" =~ single_version_override.*module_name[[:space:]]*=[[:space:]]*[\'\"]*protobuf[\'\"]*.*version ]] && ! [[ "$line" =~ ^[[:space:]]*# ]]; then
        # Replace version using single quote format uniformly
        new_line=$(echo "$line" | sed "s/version[[:space:]]*=[[:space:]]*['\"][^'\"]*['\"]/version = '$NEW_VERSION'/")
        echo "$new_line"
    else
        echo "$line"
        # If no override exists and current line is bazel_dep protobuf, add single_version_override after it
        if [ "$override_exists" = false ] && [ "$current_line_num" -eq "$bazel_dep_line_num" ]; then
            echo "single_version_override(module_name = 'protobuf', version = '$NEW_VERSION')"
        fi
    fi
done < "$MODULE_FILE" > "$TEMP_FILE"

# Replace original file directly
mv "$TEMP_FILE" "$MODULE_FILE"

echo "Successfully updated protobuf version to $NEW_VERSION in $MODULE_FILE"

cat "$MODULE_FILE"

# Clean up temporary file
trap "rm -f $TEMP_FILE" EXIT
