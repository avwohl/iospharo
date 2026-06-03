#!/usr/bin/env bash
# load-creds.sh — parse AWS creds out of ~/.ssh/aws.txt into the environment.
#
# Accepts either:
#   (a) the freeform line used in this project, e.g.
#         aws
#         IAM access AKIA... secret <secret>
#   (b) standard "key = value" lines (aws_access_key_id / aws_secret_access_key).
#
# Usage:  source scripts/aws/load-creds.sh   (exports AWS_ACCESS_KEY_ID etc.)

_aws_txt="${AWS_TXT:-$HOME/.ssh/aws.txt}"
if [ ! -r "$_aws_txt" ]; then
    echo "load-creds: cannot read $_aws_txt" >&2
    return 1 2>/dev/null || exit 1
fi

_raw="$(cat "$_aws_txt")"

# Access key id: the only AKIA/ASIA-prefixed 20-char token.
AWS_ACCESS_KEY_ID="$(printf '%s\n' "$_raw" | grep -oE '(AKIA|ASIA)[A-Z0-9]{16}' | head -1)"

# Secret: token immediately following the word 'secret' (format a),
# else the value of an aws_secret_access_key line (format b).
AWS_SECRET_ACCESS_KEY="$(printf '%s\n' "$_raw" \
    | sed -nE 's/.*[Ss]ecret[[:space:]]+([A-Za-z0-9/+=]{30,}).*/\1/p' | head -1)"
if [ -z "$AWS_SECRET_ACCESS_KEY" ]; then
    AWS_SECRET_ACCESS_KEY="$(printf '%s\n' "$_raw" \
        | sed -nE 's/^[[:space:]]*aws_secret_access_key[[:space:]]*=[[:space:]]*([^[:space:]]+).*/\1/p' | head -1)"
fi

if [ -z "$AWS_ACCESS_KEY_ID" ] || [ -z "$AWS_SECRET_ACCESS_KEY" ]; then
    echo "load-creds: failed to parse access key / secret from $_aws_txt" >&2
    return 1 2>/dev/null || exit 1
fi

export AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY
export AWS_DEFAULT_REGION="${AWS_DEFAULT_REGION:-us-east-1}"
# Make sure brew's aws is on PATH for callers that didn't set it.
case ":$PATH:" in *":/opt/homebrew/bin:"*) ;; *) export PATH="/opt/homebrew/bin:$PATH";; esac
echo "load-creds: loaded access key ${AWS_ACCESS_KEY_ID:0:8}… region $AWS_DEFAULT_REGION"
