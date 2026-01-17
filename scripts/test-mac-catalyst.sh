#!/bin/bash
#
# test-mac-catalyst.sh - Automated GUI test for Mac Catalyst app
#
# Tests that the app launches, renders, responds to mouse clicks,
# and exits cleanly when Quit is selected from the Pharo menu.
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

APP_PATH="/Users/wohl/Library/Developer/Xcode/DerivedData/iospharo-buxermqmwwmdxjbbgetrozfkwrod/Build/Products/Debug-maccatalyst/iospharo.app"
APP_NAME="iospharo"
LOG_FILE="/tmp/iospharo-test.log"

# Check dependencies
check_dependencies() {
    if ! command -v cliclick &> /dev/null; then
        echo -e "${RED}Error: cliclick not found. Install with: brew install cliclick${NC}"
        exit 1
    fi
    if ! command -v osascript &> /dev/null; then
        echo -e "${RED}Error: osascript not found (should be part of macOS)${NC}"
        exit 1
    fi
}

# Kill any existing instances
cleanup() {
    pkill -9 "$APP_NAME" 2>/dev/null || true
    rm -f "$LOG_FILE"
}

# Build the app
build_app() {
    echo -e "${YELLOW}Building Mac Catalyst app...${NC}"
    xcodebuild -project iospharo.xcodeproj \
        -scheme iospharo \
        -configuration Debug \
        -destination 'platform=macOS,variant=Mac Catalyst' \
        build 2>&1 | grep -E "(BUILD|error:|warning:)" || true

    if [ ! -d "$APP_PATH" ]; then
        echo -e "${RED}Build failed: app not found at $APP_PATH${NC}"
        exit 1
    fi
    echo -e "${GREEN}Build succeeded${NC}"
}

# Launch the app
launch_app() {
    echo -e "${YELLOW}Launching app...${NC}"
    "$APP_PATH/Contents/MacOS/$APP_NAME" 2>&1 | tee "$LOG_FILE" &
    APP_PID=$!
    echo "App PID: $APP_PID"

    # Wait for app to start and render
    sleep 5

    # Check if app is still running
    if ! kill -0 $APP_PID 2>/dev/null; then
        echo -e "${RED}App crashed on startup${NC}"
        cat "$LOG_FILE"
        exit 1
    fi

    echo -e "${GREEN}App launched successfully${NC}"
}

# Get window position
get_window_position() {
    local pos
    pos=$(osascript -e "tell application \"System Events\" to tell process \"$APP_NAME\" to get position of window 1" 2>/dev/null)
    if [ -z "$pos" ]; then
        echo -e "${RED}Could not get window position${NC}"
        exit 1
    fi
    WIN_X=$(echo "$pos" | cut -d, -f1 | tr -d ' ')
    WIN_Y=$(echo "$pos" | cut -d, -f2 | tr -d ' ')
    echo "Window position: ($WIN_X, $WIN_Y)"
}

# Activate the app window
activate_window() {
    echo -e "${YELLOW}Activating window...${NC}"
    osascript -e "tell application \"$APP_NAME\" to activate"
    sleep 0.5
}

# Click at window-relative coordinates
click_at() {
    local rel_x=$1
    local rel_y=$2
    local desc=$3

    local screen_x=$((WIN_X + rel_x))
    local screen_y=$((WIN_Y + rel_y))

    echo "  Clicking at ($rel_x, $rel_y) [screen: ($screen_x, $screen_y)] - $desc"
    cliclick c:$screen_x,$screen_y
    sleep 0.3
}

# Test: Menu quit
test_menu_quit() {
    echo -e "${YELLOW}Testing Pharo menu quit...${NC}"

    # Click on Pharo menu (in menu bar at y ~= 50)
    click_at 50 50 "Pharo menu"

    # Wait for menu to open
    sleep 0.5

    # Click on Quit (last item in dropdown, at y ~= 252)
    click_at 50 252 "Quit menu item"

    # Wait for app to process quit
    sleep 2

    # Check if app has quit
    if pgrep -f "$APP_NAME.app" > /dev/null; then
        echo -e "${RED}FAIL: App is still running after Quit${NC}"
        return 1
    fi

    # Verify the quit was via our intercept
    if grep -q "Intercepted snapshot:andQuit:" "$LOG_FILE"; then
        echo -e "${GREEN}PASS: VM intercepted snapshot:andQuit:${NC}"
    else
        echo -e "${YELLOW}WARN: Did not see snapshot:andQuit: intercept in logs${NC}"
    fi

    if grep -q "VM stopped, exiting app" "$LOG_FILE"; then
        echo -e "${GREEN}PASS: App exited cleanly${NC}"
    else
        echo -e "${YELLOW}WARN: Did not see clean exit message${NC}"
    fi

    return 0
}

# Test: Mouse events are captured
test_mouse_events() {
    echo -e "${YELLOW}Testing mouse event capture...${NC}"

    # Click in the middle of the canvas
    click_at 500 400 "Canvas center"

    sleep 0.5

    # Check if touch events were logged
    if grep -q "\[TOUCH\] touchesBegan" "$LOG_FILE"; then
        echo -e "${GREEN}PASS: Touch events are being captured${NC}"
        return 0
    else
        echo -e "${RED}FAIL: No touch events in log${NC}"
        return 1
    fi
}

# Main test runner
main() {
    local failed=0

    echo "========================================"
    echo "  Mac Catalyst GUI Test"
    echo "========================================"
    echo

    check_dependencies
    cleanup

    # Optionally skip build with --no-build
    if [[ "$1" != "--no-build" ]]; then
        build_app
    fi

    launch_app
    activate_window
    get_window_position

    echo
    echo "Running tests..."
    echo

    # Test 1: Mouse events
    if ! test_mouse_events; then
        ((failed++))
    fi

    echo

    # Test 2: Menu quit (this will exit the app)
    if ! test_menu_quit; then
        ((failed++))
    fi

    echo
    echo "========================================"
    if [ $failed -eq 0 ]; then
        echo -e "  ${GREEN}All tests passed!${NC}"
    else
        echo -e "  ${RED}$failed test(s) failed${NC}"
    fi
    echo "========================================"

    # Cleanup any remaining processes
    cleanup

    exit $failed
}

# Run main with all arguments
main "$@"
