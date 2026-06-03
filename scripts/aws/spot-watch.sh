#!/usr/bin/env bash
# spot-watch.sh — long-running watcher (iospharo-spot-watch.service).  AWS
# publishes a spot-interruption notice ~2 min before reclaiming the instance.
# When it appears, we immediately preserve state (git push + S3 sync) so the
# 2-minute window isn't wasted.
set -uo pipefail
PRESERVE=/opt/iospharo/preserve.sh

while true; do
    TOKEN=$(curl -fsS -X PUT "http://169.254.169.254/latest/api/token" \
        -H "X-aws-ec2-metadata-token-ttl-seconds: 60" 2>/dev/null || true)
    code=$(curl -s -o /dev/null -w '%{http_code}' \
        -H "X-aws-ec2-metadata-token: $TOKEN" \
        http://169.254.169.254/latest/meta-data/spot/instance-action 2>/dev/null || echo 000)
    if [ "$code" = "200" ]; then
        echo "spot-watch: interruption notice received — preserving NOW"
        [ -x "$PRESERVE" ] && "$PRESERVE" "spot-interruption" || true
        # Keep trying to preserve until the box dies.
        sleep 20
    fi
    sleep 5
done
