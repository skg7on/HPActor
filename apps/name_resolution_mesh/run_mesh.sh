#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
BINARY="$BUILD_DIR/apps/name_resolution_mesh/19_name_resolution_mesh"
LOG_DIR="$BUILD_DIR/mesh-logs"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

cleanup() {
    echo -e "\n${YELLOW}Shutting down...${NC}"
    for pid in "${PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    echo -e "${YELLOW}Logs saved to: $LOG_DIR/${NC}"
    ls -la "$LOG_DIR"/node-*.log 2>/dev/null || true
    exit 0
}
trap cleanup SIGINT SIGTERM

echo -e "${CYAN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║     Name Resolution Mesh — 3-Node Distributed Demo      ║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

# Build if needed
if [[ ! -x "$BINARY" ]]; then
    echo -e "${YELLOW}Building 19_name_resolution_mesh...${NC}"
    (cd "$REPO_ROOT" && ninja -C build 19_name_resolution_mesh)
fi

# Prepare log directory
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"

PIDS=()
echo -e "${CYAN}Launching 3 nodes...${NC}"

# Launch payment node first
"$BINARY" --role payment   --base-port 10001 --delay-ms 2000 \
    > "$LOG_DIR/node-payment.log" 2>&1 &
PIDS+=($!)

# Launch inventory node
"$BINARY" --role inventory --base-port 10001 --delay-ms 2000 \
    > "$LOG_DIR/node-inventory.log" 2>&1 &
PIDS+=($!)

# Launch gateway node (drives most scenarios)
"$BINARY" --role gateway   --base-port 10001 --delay-ms 2000 \
    > "$LOG_DIR/node-gateway.log" 2>&1 &
PIDS+=($!)

echo "  PIDs: gateway=${PIDS[2]}, payment=${PIDS[0]}, inventory=${PIDS[1]}"
echo "  Logs: $LOG_DIR/"
echo ""

# Wait for all nodes to complete or timeout
WAIT_TIME=120
ELAPSED=0
ALL_DONE=false

while [[ $ELAPSED -lt $WAIT_TIME ]]; do
    ALL_DONE=true
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            ALL_DONE=false
            break
        fi
    done
    if $ALL_DONE; then break; fi
    sleep 2
    ELAPSED=$((ELAPSED + 2))
done

if ! $ALL_DONE; then
    echo -e "${YELLOW}Timeout — some processes still running. Killing...${NC}"
    cleanup
fi

# Reap all PIDs
for pid in "${PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done

echo ""
echo -e "${CYAN}Results:${NC}"
for role in gateway payment inventory; do
    log="$LOG_DIR/node-$role.log"
    if [[ -f "$log" ]]; then
        last_line=$(tail -1 "$log" 2>/dev/null || echo "incomplete")
        if echo "$last_line" | grep -q "FAILED"; then
            echo -e "  ${RED}$role: $last_line${NC}"
        elif echo "$last_line" | grep -q "passed"; then
            echo -e "  ${GREEN}$role: $last_line${NC}"
        else
            echo -e "  ${YELLOW}$role: $last_line${NC}"
        fi
    else
        echo -e "  ${RED}$role: no log file${NC}"
    fi
done

echo ""
echo -e "${CYAN}Full logs:${NC}"
for role in gateway payment inventory; do
    echo "  $LOG_DIR/node-$role.log"
done
