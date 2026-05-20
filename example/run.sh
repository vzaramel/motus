#!/bin/bash
#
# Run the Motus Example
#
# This script starts the origin server and optionally the edge worker
# for local development and testing.
#
# Usage:
#   ./run.sh          - Start both origin and edge
#   ./run.sh origin   - Start only origin server
#   ./run.sh test     - Quick test of origin server
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

ORIGIN_PID=""
EDGE_PID=""
STEP_PID=""
CLEANUP_RAN=0

stop_process_tree() {
    local pid="$1"
    local name="$2"
    if [[ -z "$pid" ]]; then
        return 0
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        return 0
    fi

    if command -v pgrep >/dev/null 2>&1; then
        local children
        children="$(pgrep -P "$pid" 2>/dev/null || true)"
        for child in $children; do
            stop_process_tree "$child" "$name child"
        done
    fi

    echo -e "${BLUE}Stopping ${name} (pid ${pid})...${NC}"
    kill "$pid" 2>/dev/null || true
    for _ in 1 2 3 4 5; do
        if ! kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
    done
    kill -9 "$pid" 2>/dev/null || true
}

cleanup() {
    if [[ "$CLEANUP_RAN" -eq 1 ]]; then
        return 0
    fi
    CLEANUP_RAN=1
    echo ""
    echo -e "${BLUE}Shutting down...${NC}"
    stop_process_tree "$EDGE_PID" "edge worker"
    stop_process_tree "$ORIGIN_PID" "origin server"
    stop_process_tree "$STEP_PID" "setup step"
}
trap cleanup INT TERM HUP EXIT

run_step() {
    "$@" &
    STEP_PID=$!
    wait "$STEP_PID"
    STEP_PID=""
}

is_port_busy() {
    local port="$1"
    if command -v nc >/dev/null 2>&1; then
        nc -z 127.0.0.1 "$port" >/dev/null 2>&1
        return $?
    fi
    local code
    code="$(curl -sS -m 1 -o /dev/null -w '%{http_code}' "http://127.0.0.1:${port}/health" || true)"
    [[ "$code" != "000" ]]
}

echo -e "${BLUE}=================================${NC}"
echo -e "${BLUE}Motus Example Runner${NC}"
echo -e "${BLUE}=================================${NC}"
echo ""

# Build the main library if needed
echo -e "${GREEN}Building Motus library...${NC}"
make -C .. lib

# Build the origin server
echo -e "${GREEN}Building origin server...${NC}"
make -C origin

# Parse mode argument
ORIGIN_PORT="${ORIGIN_PORT:-8099}"
EDGE_PORT="${EDGE_PORT:-8791}"
MOT_VM="${MOT_VM:-wasm}"
MOT_DEBUG="${MOT_DEBUG:-0}"
MOT_DEV="${MOT_DEV:-1}"
MOT_HOT="${MOT_HOT:-0}"
MOT_BROWSER_WASM="${MOT_BROWSER_WASM:-1}"
ORIGIN_URL="${ORIGIN_URL:-}"
ORIGIN_URL_EXPLICIT=0
if [[ -n "$ORIGIN_URL" ]]; then
    ORIGIN_URL_EXPLICIT=1
fi
MODE="both"

if [[ $# -gt 0 && "${1#-}" = "$1" ]]; then
    MODE="$1"
    shift
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        --origin-port)
            ORIGIN_PORT="${2:-}"
            shift 2
            ;;
        --edge-port)
            EDGE_PORT="${2:-}"
            shift 2
            ;;
        --vm)
            MOT_VM="${2:-}"
            shift 2
            ;;
        --origin-url)
            ORIGIN_URL="${2:-}"
            ORIGIN_URL_EXPLICIT=1
            shift 2
            ;;
        --dev)
            MOT_DEV="1"
            shift
            ;;
        --no-dev)
            MOT_DEV="0"
            MOT_HOT="0"
            shift
            ;;
        --hot)
            MOT_HOT="1"
            MOT_DEV="1"
            shift
            ;;
        --live-reload)
            MOT_DEV="1"
            MOT_HOT="0"
            shift
            ;;
        --debug)
            MOT_DEBUG="1"
            shift
            ;;
        --no-debug)
            MOT_DEBUG="0"
            shift
            ;;
        --browser-wasm)
            MOT_BROWSER_WASM="1"
            shift
            ;;
        --no-browser-wasm)
            MOT_BROWSER_WASM="0"
            shift
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

if [[ "$ORIGIN_URL_EXPLICIT" -eq 0 ]]; then
    ORIGIN_URL="http://localhost:${ORIGIN_PORT}"
fi

case "$MODE" in
    test)
        # Quick test mode - start origin, test, stop
        echo ""
        echo -e "${GREEN}Running quick test...${NC}"
        cd origin
        ORIGIN_PORT="$ORIGIN_PORT" ./server &
        ORIGIN_PID=$!
        cd ..
        sleep 1

        echo ""
        echo -e "${BLUE}Testing health endpoint:${NC}"
        curl -s "http://localhost:${ORIGIN_PORT}/health" && echo ""

        echo ""
        echo -e "${BLUE}Testing page compilation:${NC}"
        curl -s -o /dev/null -w "  Status: %{http_code}, Bytecode size: %{size_download} bytes\n" "http://localhost:${ORIGIN_PORT}/page/index"

        echo ""
        echo -e "${BLUE}Bytecode header:${NC}"
        curl -s "http://localhost:${ORIGIN_PORT}/page/index" | xxd | head -5

        kill $ORIGIN_PID 2>/dev/null || true
        echo ""
        echo -e "${GREEN}Test complete!${NC}"
        exit 0
        ;;

    origin)
        # Origin only mode
        echo ""
        echo -e "${GREEN}Starting origin server on http://localhost:${ORIGIN_PORT}...${NC}"
        echo -e "${BLUE}(Edge worker not started - Node.js/npm not required)${NC}"
        cd origin
        ORIGIN_PORT="$ORIGIN_PORT" MOT_DEV="$MOT_DEV" ./server
        ;;

    both)
        if [[ "$MOT_VM" != "wasm" ]]; then
            echo -e "${RED}Unsupported --vm '${MOT_VM}'. Edge runtime currently supports only 'wasm'.${NC}"
            exit 1
        fi

        # Check if npm is available
        if ! command -v npm &> /dev/null; then
            echo -e "${RED}Warning: npm not found. Cannot start edge worker.${NC}"
            echo -e "${BLUE}Run './run.sh origin' to start only the origin server.${NC}"
            echo -e "${BLUE}To install Node.js: https://nodejs.org/${NC}"
            echo ""
            exit 1
        fi

        if is_port_busy "$ORIGIN_PORT"; then
            echo -e "${RED}Error: origin port ${ORIGIN_PORT} is already in use.${NC}"
            echo -e "${BLUE}Use --origin-port with a different value or stop the existing process.${NC}"
            exit 1
        fi
        if is_port_busy "$EDGE_PORT"; then
            echo -e "${RED}Error: edge port ${EDGE_PORT} is already in use.${NC}"
            echo -e "${BLUE}Use --edge-port with a different value or stop the existing process.${NC}"
            exit 1
        fi

        # Install edge dependencies
        echo -e "${GREEN}Installing edge dependencies...${NC}"
        cd edge
        run_step npm install --silent >/dev/null 2>&1 || true
        echo -e "${GREEN}Precompiling system components for edge bundle...${NC}"
        run_step npm run build:system-components
        echo -e "${GREEN}Building WASM runtime...${NC}"
        if [[ "$MOT_DEBUG" == "1" ]]; then
            run_step npm run build:wasm:debug
        else
            run_step npm run build:wasm
        fi
        cd ..

        # Start origin server
        echo ""
        echo -e "${GREEN}Starting origin server on http://localhost:${ORIGIN_PORT}...${NC}"
        cd origin
        ORIGIN_PORT="$ORIGIN_PORT" MOT_DEV="$MOT_DEV" ./server &
        ORIGIN_PID=$!
        cd ..

        # Wait for origin to start
        sleep 1

        # Check if origin is running
        if ! kill -0 $ORIGIN_PID 2>/dev/null; then
            echo -e "${RED}Error: Origin server failed to start${NC}"
            exit 1
        fi

        # Start edge worker
        echo -e "${GREEN}Starting edge worker on http://localhost:${EDGE_PORT} (vm=${MOT_VM})...${NC}"
        cd edge
        npx wrangler dev --local \
          --port "$EDGE_PORT" \
          --var "ORIGIN_PORT:${ORIGIN_PORT}" \
          --var "ORIGIN_URL:${ORIGIN_URL}" \
          --var "MOT_VM:${MOT_VM}" \
          --var "MOT_DEBUG:${MOT_DEBUG}" \
          --var "MOT_DEV:${MOT_DEV}" \
          --var "MOT_HOT:${MOT_HOT}" \
          --var "MOT_BROWSER_WASM:${MOT_BROWSER_WASM}" &
        EDGE_PID=$!
        cd ..

        echo ""
        echo -e "${BLUE}=================================${NC}"
        echo -e "${GREEN}Services running:${NC}"
        echo -e "  Origin: ${BLUE}${ORIGIN_URL}${NC}"
        echo -e "  Edge:   ${BLUE}http://localhost:${EDGE_PORT}${NC}"
        echo -e "  VM:     ${BLUE}${MOT_VM}${NC}"
        echo -e "  Dev:    ${BLUE}${MOT_DEV}${NC}"
        echo -e "  Hot:    ${BLUE}${MOT_HOT}${NC}"
        echo -e "  Debug:  ${BLUE}${MOT_DEBUG}${NC}"
        echo -e "  Browser WASM bootstrap: ${BLUE}${MOT_BROWSER_WASM}${NC}"
        echo ""
        echo -e "Open ${BLUE}http://localhost:${EDGE_PORT}${NC} in your browser"
        if [[ "$MOT_HOT" == "1" ]]; then
            echo -e "${GREEN}Hot mode:${NC} DSD + lit-html HMR active"
            echo -e "  Edit components in content/ and they hot-swap without page reload"
            echo -e "  Use --live-reload for full-page reload, --no-dev for production"
        elif [[ "$MOT_DEV" == "1" ]]; then
            echo -e "${GREEN}Dev mode:${NC} late binding + live reload active"
            echo -e "  Edit files in content/ and the browser will reload automatically"
            echo -e "  Use --hot for component-level HMR, --no-dev for production"
        fi
        echo -e "Press Ctrl+C to stop"
        echo -e "${BLUE}=================================${NC}"
        echo ""

        # Monitor children and stop the other process if one dies
        while true; do
            if ! kill -0 "$ORIGIN_PID" 2>/dev/null; then
                echo -e "${RED}Origin server exited unexpectedly.${NC}"
                break
            fi
            if ! kill -0 "$EDGE_PID" 2>/dev/null; then
                echo -e "${RED}Edge worker exited unexpectedly.${NC}"
                break
            fi
            sleep 1
        done
        exit 0
        ;;

    *)
        echo "Usage: $0 [both|origin|test] [options]"
        echo ""
        echo "  both    Start origin server and edge worker (default)"
        echo "  origin  Start only the origin server"
        echo "  test    Quick test of the origin server"
        echo ""
        echo "Options:"
        echo "  --origin-port <port>  Origin server port (default: 8099)"
        echo "  --edge-port <port>    Edge worker port (default: 8791)"
        echo "  --origin-url <url>    Origin URL for worker (default: http://localhost:\$ORIGIN_PORT)"
        echo "  --vm <mode>           VM mode: wasm (default: wasm)"
        echo "  --dev                 Dev mode: late binding, no cache, live reload (default)"
        echo "  --no-dev              Production mode: precompile + cache"
        echo "  --hot                 Hot mode: DSD + lit-html HMR (implies --dev)"
        echo "  --live-reload         Live reload: full page reload on change (same as --dev)"
        echo "  --debug               Enable browser debug sidebar/traces"
        echo "  --no-debug            Disable browser debug sidebar/traces (default)"
        echo "  --browser-wasm        Reserved (browser wasm/reactivity temporarily disabled)"
        echo "  --no-browser-wasm     Disable browser wasm bootstrap (default)"
        echo ""
        echo "Environment variables:"
        echo "  ORIGIN_PORT   Origin server port (default: 8099)"
        echo "  EDGE_PORT     Edge worker port (default: 8791)"
        echo "  ORIGIN_URL    Origin URL for worker (default: http://localhost:\$ORIGIN_PORT)"
        echo "  MOT_VM        VM mode: wasm (default: wasm)"
        echo "  MOT_DEV       Dev mode: 1|0 (default: 1)"
        echo "  MOT_HOT       Hot mode (DSD + HMR): 1|0 (default: 0)"
        echo "  MOT_DEBUG     Debug mode: 1|0 (default: 0)"
        echo "  MOT_BROWSER_WASM  Browser wasm bootstrap mode: 1|0 (default: 0; currently disabled)"
        exit 1
        ;;
esac
