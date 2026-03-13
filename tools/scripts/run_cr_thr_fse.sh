#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

U2_DATASET_ROOT="${U2_DATASET_ROOT:-/workspace/MANS/testdata/u2}"
U4_DATASET_ROOT="${U4_DATASET_ROOT:-/workspace/MANS/testdata/u4}"
WORKDIR="${WORKDIR:-/workspace/FiniteStateEntropy/build}"
BENCH_BIN="${BENCH_BIN:-./fse/fse_codec_bench}"
ALGO="${ALGO:-fse_ans}"               # fse_ans | fse_huffman | all
CHUNKS_MB="${CHUNKS_MB:-0}"
BENCH_WARMUP="${BENCH_WARMUP:-1}"
BENCH_ITERS="${BENCH_ITERS:-10}"

CSV_FILE="${CSV_FILE:-${SCRIPT_DIR}/${ALGO}_results.csv}"
LOG_DIR="${LOG_DIR:-${SCRIPT_DIR}/log}"
LOG_FILE="${LOG_FILE:-${LOG_DIR}/${ALGO}.log}"

mkdir -p "${LOG_DIR}"
: > "${LOG_FILE}"

bench_path=""
if [[ -x "${WORKDIR}/${BENCH_BIN#./}" ]]; then
    bench_path="${WORKDIR}/${BENCH_BIN#./}"
elif [[ -x "${WORKDIR}/fse/fse_codec_bench" ]]; then
    bench_path="${WORKDIR}/fse/fse_codec_bench"
elif [[ -x "${WORKDIR}/fse_codec_bench" ]]; then
    bench_path="${WORKDIR}/fse_codec_bench"
else
    echo "[error] bench binary not found: ${WORKDIR}/${BENCH_BIN#./}" | tee -a "${LOG_FILE}"
    exit 1
fi

algorithms=()
case "${ALGO}" in
    fse_ans|fse_huffman)
        algorithms+=("${ALGO}")
        ;;
    all)
        algorithms+=("fse_ans" "fse_huffman")
        ;;
    *)
        echo "[error] unsupported ALGO=${ALGO}, use fse_ans|fse_huffman|all" | tee -a "${LOG_FILE}"
        exit 1
        ;;
esac

dataset_files=()
dataset_types=()
add_dataset_files() {
    local root="$1"
    local input_type="$2"
    local files=()
    if [[ ! -d "${root}" ]]; then
        echo "[warn] dataset root not found, skip: ${root}" | tee -a "${LOG_FILE}"
        return
    fi
    while IFS= read -r -d '' f; do
        files+=("${f}")
    done < <(find "${root}" -type f -print0 | sort -z)
    for f in "${files[@]}"; do
        dataset_files+=("${f}")
        dataset_types+=("${input_type}")
    done
}

add_dataset_files "${U2_DATASET_ROOT}" "-u2"
add_dataset_files "${U4_DATASET_ROOT}" "-u4"

if [[ ${#dataset_files[@]} -eq 0 ]]; then
    echo "[done] no datasets found." | tee -a "${LOG_FILE}"
    exit 0
fi

printf 'dataset_folder,dataset_name,file_size_bytes,algo,input_type,ratio,comp_mbps,decomp_mbps,error\n' > "${CSV_FILE}"

echo "[info] bench: ${bench_path}" | tee -a "${LOG_FILE}"
echo "[info] csv: ${CSV_FILE}" | tee -a "${LOG_FILE}"
echo "[info] datasets: ${#dataset_files[@]}" | tee -a "${LOG_FILE}"
echo "[info] warmup: ${BENCH_WARMUP}" | tee -a "${LOG_FILE}"
echo "[info] iters: ${BENCH_ITERS}" | tee -a "${LOG_FILE}"

for i in "${!dataset_files[@]}"; do
    dataset_file="${dataset_files[$i]}"
    input_type="${dataset_types[$i]}"
    dataset_folder="$(basename -- "$(dirname -- "${dataset_file}")")"
    dataset_name="$(basename -- "${dataset_file}")"
    file_size_bytes="$(stat -c%s "${dataset_file}")"

    for algo_name in "${algorithms[@]}"; do
        tmp_csv="$(mktemp /tmp/fse_codec_bench.XXXXXX.csv)"
        tmp_log="$(mktemp /tmp/fse_codec_bench.XXXXXX.log)"

        echo "[run] ${algo_name} (${input_type}) ${dataset_file}" | tee -a "${LOG_FILE}"
        if "${bench_path}" "${input_type}" "${dataset_file}" --algo "${algo_name}" --chunks "${CHUNKS_MB}" --warmup "${BENCH_WARMUP}" --iters "${BENCH_ITERS}" --csv "${tmp_csv}" > "${tmp_log}" 2>&1; then
            cat "${tmp_log}" >> "${LOG_FILE}"
            wrote_rows=0
            while IFS=, read -r chunk_label chunk_bytes ratio_pct comp_mbps decomp_mbps; do
                [[ -z "${chunk_label}" ]] && continue
                ratio="$(awk -v pct="${ratio_pct}" 'BEGIN { if (pct + 0 > 0) printf "%.2f", 100.0 / pct; else print "" }')"
                printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                    "${dataset_folder}" \
                    "${dataset_name}" \
                    "${file_size_bytes}" \
                    "${algo_name}" \
                    "${input_type}" \
                    "${ratio}" \
                    "${comp_mbps}" \
                    "${decomp_mbps}" \
                    "" >> "${CSV_FILE}"
                wrote_rows=1
            done < <(awk 'NR > 1' "${tmp_csv}")

            if [[ "${wrote_rows}" -eq 0 ]]; then
                printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                    "${dataset_folder}" \
                    "${dataset_name}" \
                    "${file_size_bytes}" \
                    "${algo_name}" \
                    "${input_type}" \
                    "" "" "" \
                    "empty_result_csv" >> "${CSV_FILE}"
            fi
        else
            cat "${tmp_log}" >> "${LOG_FILE}"
            err="$(awk '
                /Decompression mismatch|Compression failed|invalid|overflow|Error|failed|core dumped/ { msg=$0 }
                END { if (msg != "") print msg; else print "bench_command_failed" }
            ' "${tmp_log}" | tail -n 1)"
            err="${err//,/;}"
            printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                "${dataset_folder}" \
                "${dataset_name}" \
                "${file_size_bytes}" \
                "${algo_name}" \
                "${input_type}" \
                "" "" "" \
                "${err}" >> "${CSV_FILE}"
        fi

        rm -f "${tmp_csv}" "${tmp_log}"
    done
done

echo "[done] csv=${CSV_FILE}" | tee -a "${LOG_FILE}"
