#!/bin/sh
set -eu
while true; do
  stamp="$(date -u +%Y%m%dT%H%M%SZ)"
  pg_dump --format=custom --file="/backups/community-${stamp}.dump"
  find /backups -type f -name 'community-*.dump' -mtime "+${BACKUP_RETENTION_DAYS}" -delete
  sleep 86400
done

