#!/bin/bash
#
# build.sh - Build script for lvgl_panel on Raspberry Pi
#
# Usage:
#   ./build.sh              # Sync to Pi and build (from dev machine) or build locally (on Pi)
#   ./build.sh deps         # Install dependencies (on Pi)
#   ./build.sh clean        # Clean build artifacts
#   ./build.sh install      # Install binary and service (on Pi)
#   ./build.sh --install    # Build and install in one step (on Pi)
#   ./build.sh help         # Show this help
#

set -e

# Remote Pi configuration
REMOTE_HOST="${REMOTE_HOST:-192.168.1.50}"
REMOTE_DIR="${REMOTE_DIR:-lvgl_panel}"

# Configuration
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_NAME="panel"

# Compiler settings
CC="${CC:-gcc}"
CXX="${CXX:-g++}"

# Detect platform
ARCH=$(uname -m)
OS=$(uname -s)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if running as root
check_root() {
    if [ "$EUID" -ne 0 ]; then
        return 1
    fi
    return 0
}

# Run a command, prefixing with sudo if not root
run_root() {
    if ! check_root; then
        sudo "$@"
    else
        "$@"
    fi
}

# Sync to remote Pi and build there
remote_build() {
    log_info "Remote build on ${REMOTE_HOST}"
    echo "========================================"
    echo "  lvgl_panel Remote Build"
    echo "========================================"
    echo "Remote host: ${REMOTE_HOST}"
    echo "Destination: ${REMOTE_DIR}"
    echo "========================================"
    echo ""

    # Rsync excludes
    EXCLUDES=(
        --exclude='.git'
        --exclude='.claude'
        --exclude='obj/'
        --exclude='*.o'
        --exclude='.DS_Store'
        --exclude='design/'
        --exclude='gallery/'
        --exclude='info/'
        --exclude="${BIN_NAME}"
    )

    log_info "Syncing files to ${REMOTE_HOST}:${REMOTE_DIR} ..."
    rsync -avz --delete "${EXCLUDES[@]}" "${PROJECT_DIR}/" "${REMOTE_HOST}:${REMOTE_DIR}/"

    # Sync panel.ini if it exists locally
    if [ -f "${PROJECT_DIR}/panel.ini" ]; then
        log_info "Syncing panel.ini..."
        rsync -avz "${PROJECT_DIR}/panel.ini" "${REMOTE_HOST}:${REMOTE_DIR}/panel.ini"
    fi

    REMOTE_CMD="./build.sh app"
    if [ "${REMOTE_INSTALL:-false}" = true ]; then
        REMOTE_CMD="./build.sh --install"
    fi

    log_info "Building on remote..."
    ssh -t "${REMOTE_HOST}" "cd ${REMOTE_DIR} && ${REMOTE_CMD}"

    echo ""
    log_info "Build complete!"
    if [ "${REMOTE_INSTALL:-false}" != true ]; then
        echo ""
        echo "To run on the Pi:"
        echo "  ssh ${REMOTE_HOST}"
        echo "  cd ${REMOTE_DIR}"
        echo "  sudo ./${BIN_NAME}"
    fi
}

# Install system dependencies
install_deps() {
    log_info "Installing system dependencies..."

    if [ "${OS}" != "Linux" ]; then
        log_error "Dependencies can only be installed on Linux/Raspberry Pi"
        exit 1
    fi

    if command -v apt-get &> /dev/null; then
        INSTALL_CMD="apt-get install -y"
        UPDATE_CMD="apt-get update"
    elif command -v yum &> /dev/null; then
        INSTALL_CMD="yum install -y"
        UPDATE_CMD="yum check-update || true"
    elif command -v pacman &> /dev/null; then
        INSTALL_CMD="pacman -S --noconfirm"
        UPDATE_CMD="pacman -Sy"
    else
        log_error "Unknown package manager. Please install dependencies manually."
        exit 1
    fi

    if ! check_root; then
        INSTALL_CMD="sudo ${INSTALL_CMD}"
        UPDATE_CMD="sudo ${UPDATE_CMD}"
    fi

    log_info "Updating package lists..."
    ${UPDATE_CMD}

    log_info "Installing build tools and libraries..."
    ${INSTALL_CMD} build-essential pkg-config libcurl4-openssl-dev libpng-dev libbsd-dev

    log_info "Dependencies installed successfully"
}

# Build panel application
build_panel() {
    log_info "Building ${BIN_NAME}..."
    log_info "Platform: ${OS} ${ARCH}"

    cd "${PROJECT_DIR}"

    # Pass version info via CFLAGS
    GIT_HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
    BUILD_DATE=$(date +%Y-%m-%d)
    export EXTRA_CFLAGS="-DPANEL_BUILD_DATE=\"${BUILD_DATE}\" -DPANEL_BUILD_HASH=\"${GIT_HASH}\""
    log_info "Date: ${BUILD_DATE}, Hash: ${GIT_HASH}"

    make -j$(nproc 2>/dev/null || echo 4)

    log_info "Build complete!"
    echo ""
    echo "Binary: ${PROJECT_DIR}/${BIN_NAME}"
    echo ""
    echo "To run:"
    echo "  sudo ./${BIN_NAME}"
    echo ""
}

# Clean build artifacts
clean() {
    log_info "Cleaning build artifacts..."
    cd "${PROJECT_DIR}"
    make clean
    rm -f "${PROJECT_DIR}/${BIN_NAME}"
    log_info "Clean complete"
}

# Install application and service
install_app() {
    log_info "Installing ${BIN_NAME}..."

    if [ "${OS}" != "Linux" ]; then
        log_error "Install can only be run on Linux/Raspberry Pi"
        exit 1
    fi

    if [ ! -f "${PROJECT_DIR}/${BIN_NAME}" ]; then
        log_error "Binary not found. Run './build.sh app' first."
        exit 1
    fi

    INSTALL_DIR="/var/app"
    SERVICE_NAME="photoframe.service"
    SERVICE_WAS_RUNNING=false

    # Always stop service before overwriting binary (handles active, restarting, etc.)
    log_info "Stopping service..."
    run_root systemctl stop "${SERVICE_NAME}" 2>/dev/null && SERVICE_WAS_RUNNING=true
    # Wait for process to fully release the binary
    for i in 1 2 3 4 5; do
        if ! pgrep -x "${BIN_NAME}" >/dev/null 2>&1; then
            break
        fi
        log_info "Waiting for process to exit..."
        sleep 1
    done
    # Force kill if still running
    if pgrep -x "${BIN_NAME}" >/dev/null 2>&1; then
        log_warn "Process did not exit, killing..."
        run_root pkill -9 -x "${BIN_NAME}" 2>/dev/null
        sleep 1
    fi

    # Ensure install directory exists
    run_root mkdir -p "${INSTALL_DIR}"

    # Copy binary and runtime files
    log_info "Installing to ${INSTALL_DIR}..."
    run_root cp "${PROJECT_DIR}/${BIN_NAME}" "${INSTALL_DIR}/"
    run_root chmod +x "${INSTALL_DIR}/${BIN_NAME}"
    [ -f "${PROJECT_DIR}/panel.ini" ] && run_root cp "${PROJECT_DIR}/panel.ini" "${INSTALL_DIR}/"

    # Install systemd service if service file exists
    SERVICE_FILE="${PROJECT_DIR}/service/photoframe.service"
    if [ -f "${SERVICE_FILE}" ]; then
        log_info "Installing systemd service..."
        run_root cp "${SERVICE_FILE}" /etc/systemd/system/
        run_root systemctl daemon-reload
        run_root systemctl enable "${SERVICE_NAME}"
        log_info "Service installed and enabled"
    else
        log_warn "No service file found at ${SERVICE_FILE}, skipping systemd setup"
    fi

    # Restart service
    log_info "Restarting service..."
    run_root systemctl restart "${SERVICE_NAME}"
    log_info "Service restarted"

    log_info "Installation complete!"
    echo ""
    echo "Installed to: ${INSTALL_DIR}/${BIN_NAME}"
    echo ""
}

# Show help
show_help() {
    cat << EOF
lvgl_panel Build Script

Usage: ./build.sh [command]

Commands:
  (none)      Auto-detect: remote build from dev machine, local build on Pi
  app         Build panel application (on Pi)
  install     Install binary to /var/app/lvgl_panel (on Pi)
  --install   Build and install in one step (on Pi)
  deps        Install system dependencies (on Pi)
  clean       Clean build artifacts
  help        Show this help message

Environment variables:
  REMOTE_HOST   Remote Pi address (default: 192.168.1.50)
  REMOTE_DIR    Remote directory (default: lvgl_panel)
  CC            C compiler (default: gcc)
  CXX           C++ compiler (default: g++)

Examples:
  ./build.sh                              # Auto build (remote or local)
  ./build.sh --install                    # Build and install (on Pi)
  ./build.sh deps                         # Install deps (on Pi)
  REMOTE_HOST=pi@mypi.local ./build.sh    # Build on specific Pi

EOF
}

# Main entry point
case "${1:-}" in
    deps)
        install_deps
        ;;
    app)
        build_panel
        ;;
    install)
        install_app
        ;;
    --install)
        if [ "${OS}" != "Linux" ]; then
            REMOTE_INSTALL=true remote_build
        else
            build_panel
            echo ""
            install_app
        fi
        ;;
    clean)
        clean
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        # Auto-detect: if not on Linux, do remote build
        if [ "${OS}" != "Linux" ]; then
            remote_build
        else
            build_panel
        fi
        ;;
esac
