#!/bin/sh
set -eu

has_listener=false
for argument in "$@"; do
  case "$argument" in
    --listen|--listen=*|--trusted-listen|--trusted-listen=*|\
    --tcp-listen|--tcp-listen=*|--bind|--bind=*|--port|--port=*)
      has_listener=true
      ;;
  esac
done

if [ "$has_listener" = false ]; then
  container_name=$(cat /etc/hostname)
  address_records=$(getent ahostsv4 "$container_name") || {
    echo "goblin-core: unable to resolve the container network address" >&2
    exit 70
  }
  container_address=${address_records%%[[:space:]]*}
  if [ -z "$container_address" ]; then
    echo "goblin-core: container network address is empty" >&2
    exit 70
  fi
  set -- --trusted-listen "${container_address}:6379" "$@"
fi

exec /usr/local/bin/goblin-core "$@"
