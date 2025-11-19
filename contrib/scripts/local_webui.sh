#!/bin/bash

cd $HOME

export OFFLINE_MODE="True"                                     
export ENABLE_PERSISTENT_CONFIG="False"
export ENABLE_OPENAI_API="True"
export ENABLE_OLLAMA_API="False"
export ENABLE_FOLLOW_UP_GENERATION="False"
export ENABLE_CODE_EXECUTION="False"
export ENABLE_CODE_INTERPRETER="False"
export ENABLE_DIRECT_CONNECTIONS="False"
export ENABLE_TITLE_GENERATION="False"
export ENABLE_TAGS_GENERATION="False"
export ENABLE_AUTOCOMPLETE_GENERATION="False"
export ENABLE_EVALUATION_ARENA_MODELS="False"
export ENABLE_MESSAGE_RATING="False"                     
export ENABLE_COMMUNITY_SHARING="False"
export ENABLE_REALTIME_CHAT_SAVE="True"
export OPENAI_API_BASE_URL="http://127.0.0.1:9990/v1"

export DATA_DIR=$HOME/webui-data
mkdir -p $DATA_DIR

uvx --python 3.11 open-webui@latest serve --port 8000

