#!/usr/bin/env bash
#
# Copyright 2026 NXP
#
# NXP Proprietary. This software is owned or controlled by NXP and may only be
# used strictly in accordance with the applicable license terms. By expressly
# accepting such terms or by downloading, installing, activating and/or
# otherwise using the software, you are agreeing that you have read, and that
# you agree to comply with and are bound by, such license terms. If you do not
# agree to be bound by the applicable license terms, then you may not retain,
# install, activate or otherwise use the software.
#

# ============================================================================
# Strict Error Handling
# ============================================================================
set -e          # Exit on any error
set -u          # Exit on undefined variable
set -o pipefail # Exit on pipe failure

# ============================================================================
# Configuration
# ============================================================================
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly TOOLCHAIN_PATH="${1:-}"
readonly PACKAGE_DIR="ara2-vision-examples"
readonly MULTISTREAM_YOLOV8N_APP_DIR="tasks/object-detection/yolov8n/multistream-gstreamer"
readonly DEB_PACKAGE_NAME="${PACKAGE_DIR}.deb"

# Detect if terminal supports colors
if [ -t 1 ] && command -v tput &> /dev/null && tput colors &> /dev/null && [ "$(tput colors)" -ge 8 ]; then
    readonly RED='\033[1;31m'
    readonly GREEN='\033[1;32m'
    readonly YELLOW='\033[1;33m'
    readonly BLUE='\033[1;34m'
    readonly NC='\033[0m'
else
    readonly RED=''
    readonly GREEN=''
    readonly YELLOW=''
    readonly BLUE=''
    readonly NC=''
fi

# Build configuration
readonly BUILD_TYPE="${BUILD_TYPE:-Release}"
readonly INSTALL_PREFIX="${INSTALL_PREFIX:-/usr}"
readonly NUM_JOBS="${NUM_JOBS:-$(nproc)}"

# ============================================================================
# Cleanup Handler
# ============================================================================
cleanup() {
    local exit_code=$?
    cd "$SCRIPT_DIR" || true
    
    if [ $exit_code -ne 0 ]; then
        print_error "Build failed with exit code $exit_code"
    fi
    
    exit $exit_code
}

trap cleanup EXIT INT TERM

# ============================================================================
# Helper Functions
# ============================================================================
print_step() {
    echo -e "${BLUE}==>${NC} ${GREEN}$1${NC}"
}

print_error() {
    echo -e "${RED}ERROR:${NC} $1" >&2
}

print_warning() {
    echo -e "${YELLOW}WARNING:${NC} $1"
}

print_info() {
    echo -e "${BLUE}INFO:${NC} $1"
}

check_command() {
    if ! command -v "$1" &> /dev/null; then
        print_error "Required command '$1' not found. Please install it first."
        exit 1
    fi
}

check_directory() {
    local dir=$1
    local description=${2:-"Directory"}
    
    if [ ! -d "$dir" ]; then
        print_error "$description '$dir' not found"
        exit 1
    fi
}

check_file() {
    local file=$1
    local description=${2:-"File"}
    
    if [ ! -f "$file" ]; then
        print_error "$description '$file' not found"
        exit 1
    fi
}

# Build a cmake project
build_cmake_project() {
    local project_name=$1
    local source_dir=$2
    local install_destdir=${3:-}
    local skip_install=${4:-false}
    local extra_cmake_args=${5:-}
    
    print_step "Building $project_name..."
    
    cd "$SCRIPT_DIR/$source_dir" || {
        print_error "Failed to navigate to $source_dir"
        exit 1
    }
    
    # Clean and create build directory
    rm -rf build
    mkdir -p build
    cd build
    
    # Configure
    local cmake_cmd="cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    cmake_cmd="$cmake_cmd -DCMAKE_INSTALL_PREFIX=$INSTALL_PREFIX"
    
    if [ -n "$extra_cmake_args" ]; then
        cmake_cmd="$cmake_cmd $extra_cmake_args"
    fi
    
    cmake_cmd="$cmake_cmd .."
    
    print_info "Running: $cmake_cmd"
    eval "$cmake_cmd" || {
        print_error "CMake configuration failed for $project_name"
        exit 1
    }
    
    # Build
    print_info "Compiling with $NUM_JOBS parallel jobs..."
    make -j"$NUM_JOBS" || {
        print_error "Compilation of $project_name failed"
        exit 1
    }
    
    # Install
    if [ "$skip_install" = "false" ]; then
        if [ -n "$install_destdir" ]; then
            print_info "Installing to DESTDIR=$install_destdir"
            make install DESTDIR="$install_destdir" || {
                print_error "Installation of $project_name failed"
                exit 1
            }
        else
            make install || {
                print_error "Installation of $project_name failed"
                exit 1
            }
        fi
    else
        print_info "Skipping installation (build artifacts available in build directory)"
    fi
    
    cd "$SCRIPT_DIR"
    print_step "$project_name built successfully"
}

# ============================================================================
# Usage
# ============================================================================
usage() {
    cat << EOF
Usage: $0 <toolchain_path>

Arguments:
    toolchain_path    Path to the cross-compilation toolchain setup script

Environment Variables:
    BUILD_TYPE        Build type (default: Release)
    INSTALL_PREFIX    Installation prefix (default: /usr)
    NUM_JOBS          Number of parallel build jobs (default: nproc)

Example:
    $0 /opt/toolchain/environment-setup-aarch64
    BUILD_TYPE=Debug $0 /opt/toolchain/environment-setup-aarch64

EOF
    exit 1
}

# ============================================================================
# Validation
# ============================================================================
if [ -z "$TOOLCHAIN_PATH" ]; then
    print_error "Missing required argument: toolchain_path"
    usage
fi

if [ ! -e "$TOOLCHAIN_PATH" ]; then
    print_error "Toolchain path '$TOOLCHAIN_PATH' does not exist"
    exit 1
fi

if [ ! -r "$TOOLCHAIN_PATH" ]; then
    print_error "Toolchain path '$TOOLCHAIN_PATH' is not readable"
    exit 1
fi

# Check required commands
print_step "Checking required dependencies..."
check_command cmake
check_command make
check_command dpkg-deb
check_command nproc

# Validate directory structure
print_step "Validating directory structure..."
check_directory "$MULTISTREAM_YOLOV8N_APP_DIR" "Multistream YOLOv8n application directory"
check_directory "$PACKAGE_DIR/DEBIAN" "DEBIAN control directory"
check_file "$PACKAGE_DIR/DEBIAN/control" "DEBIAN control file"
check_file "LICENSE.txt" "License file"
# ============================================================================
# Source Toolchain
# ============================================================================
print_step "Sourcing toolchain from '$TOOLCHAIN_PATH'..."

# Temporarily disable 'set -u' as toolchain scripts may have unbound variables
set +u

# shellcheck disable=SC1090
source "$TOOLCHAIN_PATH" || {
    print_error "Failed to source toolchain"
    exit 1
}

# Re-enable strict mode
set -u

# Verify cross-compilation is set up
if [ -n "${CROSS_COMPILE:-}" ]; then
    print_info "Cross-compilation prefix: $CROSS_COMPILE"
elif [ -n "${CC:-}" ]; then
    print_info "Using compiler: $CC"
else
    print_warning "No cross-compilation variables detected. Building for host?"
fi
# ============================================================================
# Build Multistream YOLOv8n Application
# ============================================================================
build_cmake_project \
    "Multistream YOLOv8n Application" \
    "$MULTISTREAM_YOLOV8N_APP_DIR" \
    "$SCRIPT_DIR/$PACKAGE_DIR" \
    "false"

# ============================================================================
# Install Documentation Files
# ============================================================================
print_step "Installing documentation files..."

# Create documentation directory following Debian policy
DOC_DIR="$PACKAGE_DIR$INSTALL_PREFIX/share/doc/${PACKAGE_DIR}"
mkdir -p "$DOC_DIR" || {
    print_error "Failed to create documentation directory"
    exit 1
}

# Install LICENSE.txt
cp "$SCRIPT_DIR/LICENSE.txt" "$DOC_DIR/LICENSE.txt" || {
    print_error "Failed to copy LICENSE.txt"
    exit 1
}
chmod 644 "$DOC_DIR/LICENSE.txt"
print_info "Installed LICENSE.txt to $INSTALL_PREFIX/share/doc/${PACKAGE_DIR}/"

# ============================================================================
# Set Permissions for DEBIAN Scripts
# ============================================================================
print_step "Setting permissions for DEBIAN scripts..."

for script in postinst postrm prerm preinst; do
    if [ -f "$PACKAGE_DIR/DEBIAN/$script" ]; then
        chmod 755 "$PACKAGE_DIR/DEBIAN/$script" || {
            print_warning "Failed to set permissions on $script"
        }
        print_info "Set permissions for $script"
    fi
done

# Ensure control file has correct permissions
chmod 644 "$PACKAGE_DIR/DEBIAN/control" || {
    print_warning "Failed to set permissions on control file"
}

# ============================================================================
# Validate Package Contents
# ============================================================================
print_step "Validating package contents..."

if [ -f "$PACKAGE_DIR$INSTALL_PREFIX/bin/multistream_yolov8" ]; then
    print_info "Multistream YOLOv8n binary found"
else
    print_error "Multistream YOLOv8n binary not found in package"
    exit 1
fi

if [ -f "$DOC_DIR/LICENSE.txt" ]; then
    print_info "LICENSE.txt found in documentation directory"
else
    print_error "LICENSE.txt not found in package"
    exit 1
fi

# ============================================================================
# Create DEB Package
# ============================================================================
print_step "Creating DEB package..."

# Remove old package if exists
[ -f "$DEB_PACKAGE_NAME" ] && rm -f "$DEB_PACKAGE_NAME"

dpkg-deb --build "$PACKAGE_DIR" "$DEB_PACKAGE_NAME" || {
    print_error "DEB package creation failed"
    exit 1
}

# Verify package was created
if [ ! -f "$DEB_PACKAGE_NAME" ]; then
    print_error "Package file was not created"
    exit 1
fi

# Show package info
PACKAGE_SIZE=$(du -h "$DEB_PACKAGE_NAME" | cut -f1)
print_info "Package size: $PACKAGE_SIZE"

# Optionally validate package
if command -v lintian &> /dev/null; then
    print_step "Running lintian checks..."
    lintian "$DEB_PACKAGE_NAME" || print_warning "Lintian found some issues (non-fatal)"
fi

# ============================================================================
# Summary
# ============================================================================
echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Build completed successfully!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo -e "Generated package: ${GREEN}$DEB_PACKAGE_NAME${NC}"
echo -e "Package size:      ${GREEN}$PACKAGE_SIZE${NC}"
echo ""
echo "To install the package, run:"
echo -e "  ${BLUE}dpkg -i $DEB_PACKAGE_NAME${NC}"
echo ""
