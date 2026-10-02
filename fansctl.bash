# fansctl bash completion — 安装到 bash-completion/completions (make install 自动放入)
_fansctl() {
  local cur sub cmds
  COMPREPLY=()
  cur="${COMP_WORDS[COMP_CWORD]}"
  sub="${COMP_WORDS[1]}"
  cmds="fans status temps power dump watch set max auto smart hold bar doctor version"

  if (( COMP_CWORD == 1 )); then
    COMPREPLY=( $(compgen -W "$cmds" -- "$cur") )
    return
  fi

  case $sub in
    fans|status|temps|power)
      [[ $cur == -* ]] && COMPREPLY=( $(compgen -W "--json" -- "$cur") ) ;;
    watch)
      [[ $cur == -* ]] && COMPREPLY=( $(compgen -W "--json" -- "$cur") ) ;;
    smart|hold)
      (( COMP_CWORD == 2 )) && COMPREPLY=( $(compgen -W "stop" -- "$cur") ) ;;
    bar)
      (( COMP_CWORD == 2 )) && COMPREPLY=( $(compgen -W "install uninstall status" -- "$cur") ) ;;
  esac
}
complete -F _fansctl fansctl
