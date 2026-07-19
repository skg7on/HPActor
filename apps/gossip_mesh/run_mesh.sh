#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
BINARY="$BUILD_DIR/apps/gossip_mesh/20_gossip_mesh"
LOG_DIR="$BUILD_DIR/gossip-logs"
BASE_PORT=15354

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
echo -e "${CYAN}║    Gossip Mesh — 3-Node SWIM Protocol Demo              ║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

# Build if needed
if [[ ! -x "$BINARY" ]]; then
    echo -e "${YELLOW}Building 20_gossip_mesh...${NC}"
    (cd "$REPO_ROOT" && ninja -C build 20_gossip_mesh)
fi

# Prepare log directory
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"

PIDS=()
echo -e "${CYAN}Launching 3 nodes...${NC}"

# ── Alpha: no seeds (solo bootstrap) ─────────────────────────────────
"$BINARY" --role alpha --base-port "$BASE_PORT" --delay-ms 3000 \
    > "$LOG_DIR/node-alpha.log" 2>&1 &
PIDS+=($!)
echo "  Alpha   PID=${PIDS[0]} (seedless solo bootstrap)"

# Give Alpha time to bind its UDP port and start the protocol timer
sleep 1

# ── Beta: seeds from Alpha ───────────────────────────────────────────
"$BINARY" --role beta --base-port "$BASE_PORT" --delay-ms 3000 \
    > "$LOG_DIR/node-beta.log" 2>&1 &
PIDS+=($!)
echo "  Beta    PID=${PIDS[1]} (seed: Alpha)"

# Give Beta time to join via Alpha and receive SyncRsp
sleep 2

# ── Gamma: seeds from Beta, discovers Alpha transitively ─────────────
"$BINARY" --role gamma --base-port "$BASE_PORT" --delay-ms 3000 \
    > "$LOG_DIR/node-gamma.log" 2>&1 &
PIDS+=($!)
echo "  Gamma   PID=${PIDS[2]} (seed: Beta)"

echo "  Logs: $LOG_DIR/"
echo ""

# ── Wait for all nodes to complete or timeout ────────────────────────
# Alpha runs all 7 scenarios including ~15s tombstone purge wait.
# Allow up to 5 minutes for worst case.
WAIT_TIME=300
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
    # Print progress every 30 seconds
    if [[ $((ELAPSED % 30)) -eq 0 ]]; then
        echo -e "${YELLOW}  Waiting... ${ELAPSED}s elapsed${NC}"
    fi
done

if ! $ALL_DONE; then
    echo -e "${YELLOW}Timeout (${WAIT_TIME}s) — some processes still running. Killing...${NC}"
    cleanup
fi

# Reap all PIDs
for pid in "${PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done

echo ""
echo -e "${CYAN}Results:${NC}"
ALL_PASSED=true
for role in alpha beta gamma; do
    log="$LOG_DIR/node-$role.log"
    if [[ -f "$log" ]]; then
        last_line=$(tail -1 "$log" 2>/dev/null || echo "incomplete")
        if echo "$last_line" | grep -q "FAILED"; then
            echo -e "  ${RED}$role: $last_line${NC}"
            ALL_PASSED=false
        elif echo "$last_line" | grep -q "passed"; then
            echo -e "  ${GREEN}$role: $last_line${NC}"
        else
            echo -e "  ${YELLOW}$role: $last_line${NC}"
            ALL_PASSED=false
        fi
    else
        echo -e "  ${RED}$role: no log file${NC}"
        ALL_PASSED=false
    fi
done

echo ""
echo -e "${CYAN}Full logs:${NC}"
for role in alpha beta gamma; do
    echo "  $LOG_DIR/node-$role.log"
done

if $ALL_PASSED; then
    echo -e "\n${GREEN}All nodes passed — gossip mesh demo complete.${NC}"
    exit 0
else
    echo -e "\n${RED}Some scenarios failed. Check logs above.${NC}"
    exit 1
fi
