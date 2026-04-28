#!/usr/bin/env bash

set -euo pipefail

MODULE_NAME="android-wuwa"
LOG_DIR="build-logs"
OUTPUT_DIR="out"
ARTIFACT_DIR="${TMPDIR:-/tmp}/android-wuwa-ko-out"

TARGETS=(
  android12-5.10
  android13-5.10
  android13-5.15
  android14-5.15
  android14-6.1
  android15-6.6
  android16-6.12
)

main() {
  cd_project_root
  check_dependencies
  prepare_dirs
  build_targets
  copy_artifacts
  print_summary
}

cd_project_root() {
  local script_dir
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  cd "${script_dir}/.."
}

check_dependencies() {
  if ! command -v ddk >/dev/null 2>&1; then
    echo "ddk 未安装，请先安装 DDK。"
    exit 1
  fi

  if ! command -v docker >/dev/null 2>&1; then
    echo "docker 未安装，请先安装 Docker。"
    exit 1
  fi

  if ! docker info >/dev/null 2>&1; then
    echo "无法访问 Docker，请检查权限或使用 sudo 运行。"
    exit 1
  fi
}

prepare_dirs() {
  rm -rf "${OUTPUT_DIR}" "${ARTIFACT_DIR}"
  mkdir -p "${OUTPUT_DIR}" "${LOG_DIR}" "${ARTIFACT_DIR}"
  : > "${LOG_DIR}/build-summary.tsv"
}

build_targets() {
  local target
  for target in "${TARGETS[@]}"; do
    build_one_target "${target}"
  done
}

build_one_target() {
  local target="$1"
  local log_file="${LOG_DIR}/${target}.log"
  local output_file="${ARTIFACT_DIR}/${MODULE_NAME}-${target}.ko"

  echo "=== ${target} ==="

  if ./scripts/build-ddk.sh build "${target}" >"${log_file}" 2>&1; then
    cp "${MODULE_NAME}.ko" "${output_file}"
    strip_module "${target}" "${output_file}" "${log_file}"
    write_success_summary "${target}" "${output_file}"
    echo "OK ${target} $(du -h "${output_file}" | cut -f1)"
  else
    write_failed_summary "${target}" "${log_file}"
    echo "FAIL ${target}"
    tail -n 80 "${log_file}"
  fi
}

strip_module() {
  local target="$1"
  local module_file="$2"
  local log_file="$3"
  local image_name
  local image_base

  image_base="$(get_image_base)"
  image_name="${image_base}:${target}"

  docker run --rm \
    -v "$(dirname "${module_file}"):/work" \
    "${image_name}" \
    llvm-strip -d "/work/$(basename "${module_file}")" >>"${log_file}" 2>&1 || true
}

get_image_base() {
  local source_file="${HOME}/.ddk/source"
  local source="github"

  if [[ -f "${source_file}" ]]; then
    source="$(cat "${source_file}")"
  fi

  case "${source}" in
    cnb)
      echo "docker.cnb.cool/ylarod/ddk/ddk"
      ;;
    docker)
      echo "docker.io/ylarod/ddk"
      ;;
    *)
      echo "ghcr.io/ylarod/ddk"
      ;;
  esac
}

write_success_summary() {
  local target="$1"
  local output_file="$2"
  local size
  local sha256
  local vermagic

  size="$(du -h "${output_file}" | cut -f1)"
  sha256="$(sha256sum "${output_file}" | awk '{print $1}')"
  vermagic="$(modinfo "${output_file}" 2>/dev/null | awk -F': *' '$1=="vermagic"{print $2; exit}')"

  printf '%s\t%s\t%s\t%s\t%s\n' \
    "${target}" "OK" "${size}" "${sha256}" "${vermagic}" \
    >> "${LOG_DIR}/build-summary.tsv"
}

write_failed_summary() {
  local target="$1"
  local log_file="$2"

  printf '%s\t%s\t%s\t%s\t%s\n' \
    "${target}" "FAIL" "-" "-" "see ${log_file}" \
    >> "${LOG_DIR}/build-summary.tsv"
}

copy_artifacts() {
  cp "${ARTIFACT_DIR}"/*.ko "${OUTPUT_DIR}/" 2>/dev/null || true
}

print_summary() {
  echo
  echo "构建结果："
  awk -F'\t' '
    BEGIN {
      printf "%-16s %-6s %-7s %-64s %s\n", "TARGET", "STATUS", "SIZE", "SHA256", "VERMAGIC"
    }
    {
      printf "%-16s %-6s %-7s %-64s %s\n", $1, $2, $3, $4, $5
    }
  ' "${LOG_DIR}/build-summary.tsv"

  echo
  echo "产物目录：${OUTPUT_DIR}"
  ls -lh "${OUTPUT_DIR}"/*.ko 2>/dev/null || true
}

main "$@"
