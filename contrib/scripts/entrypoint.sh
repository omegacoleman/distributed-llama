#!/bin/sh
set -e

DLLAMA_ROLE="${DLLAMA_ROLE:-api}"
DLLAMA_NTHREADS="${DLLAMA_THREADS:-$(nproc)}"

DLLAMA_WORKER_PORT="${DLLAMA_WORKER_PORT:-9999}"
DLLAMA_API_PORT="${DLLAMA_API_PORT:-9990}"
DLLAMA_API_WORKERS="${DLLAMA_API_WORKERS:-}"

DLLAMA_MODEL_PATH="${DLLAMA_MODEL_PATH:-/data/dllama_model_llama3.1_instruct_q40.m}"
DLLAMA_TOKENIZER_PATH="${DLLAMA_TOKENIZER_PATH:-/data/dllama_tokenizer_llama_3_1.t}"
DLLAMA_BUFFER_FLOAT_TYPE="${DLLAMA_BUFFER_FLOAT_TYPE:-q80}"
DLLAMA_MAX_SEQ_LEN="${DLLAMA_MAX_SEQ_LEN:-8192}"

if [[ "$DLLAMA_ROLE" == "worker" ]]; then
  DLLAMA_CMD="/usr/bin/dllama worker\
 --nthreads '$DLLAMA_NTHREADS'\
 --port '$DLLAMA_WORKER_PORT'"
elif [[ "$DLLAMA_ROLE" == "api" ]]; then
  DLLAMA_CMD="/usr/bin/dllama-api\
 --nthreads '$DLLAMA_NTHREADS'\
 --port '$DLLAMA_API_PORT'\
 --model '$DLLAMA_MODEL_PATH'\
 --tokenizer '$DLLAMA_TOKENIZER_PATH'\
 --buffer-float-type '$DLLAMA_BUFFER_FLOAT_TYPE'\
 --max-seq-len '$DLLAMA_MAX_SEQ_LEN'"
  if [[ "$DLLAMA_API_WORKERS" != "" ]]; then
    DLLAMA_CMD="$DLLAMA_CMD --workers '$DLLAMA_API_WORKERS'"
  fi
fi

echo "Running: $DLLAMA_CMD"
sh -c "$DLLAMA_CMD"

