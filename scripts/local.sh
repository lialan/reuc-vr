# Loads local.env from the project root (KEY=VALUE lines, "#" comments) into the environment of the caller.
# Variables that are already set win. POSIX counterpart of local.bat; source it: . "$ROOT/scripts/local.sh"
uc_local_env="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)/local.env"
if [ -f "$uc_local_env" ]; then
  while IFS= read -r uc_line || [ -n "$uc_line" ]; do
    uc_line="${uc_line%$'\r'}"
    case "$uc_line" in ''|'#'*) continue ;; esac
    uc_key="${uc_line%%=*}"
    uc_value="${uc_line#*=}"
    case "$uc_key" in *[!A-Za-z0-9_]*|'') continue ;; esac
    if [ -z "${!uc_key+x}" ]; then
      case "$uc_value" in '~/'*) uc_value="$HOME/${uc_value#\~/}" ;; esac
      export "$uc_key=$uc_value"
    fi
  done < "$uc_local_env"
fi
unset uc_local_env uc_line uc_key uc_value
