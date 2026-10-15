#!/bin/zsh
# Double-click in the morning: stops the overnight job on the PC, including its Python processes.
exec "${0:A:h}/overnight-pc.command" --stop
