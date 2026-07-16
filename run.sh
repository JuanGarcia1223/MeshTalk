#!/bin/bash

APP=$(pwd)/build/meshtak

kitty --session <(
cat <<EOF
new_tab anurag
launch env TERM=xterm-256color $APP --name juan --debug

new_tab parul
launch env TERM=xterm-256color $APP --name alex --debug

new_tab alice
launch env TERM=xterm-256color $APP --name bob --debug
EOF
)
