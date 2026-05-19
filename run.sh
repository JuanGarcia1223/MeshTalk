#!/bin/bash

APP=$(pwd)/build/meshtalk_demo

kitty --session <(
cat <<EOF
new_tab anurag
launch env TERM=xterm-256color $APP --name anurag --debug

new_tab parul
launch env TERM=xterm-256color $APP --name parul --debug

new_tab alice
launch env TERM=xterm-256color $APP --name alice --debug
EOF
)
