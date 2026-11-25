# ── A.L.I.S.H.A. shell enhancements ───────────────────────────────────────
# Sourced from ~/.zshrc. Remove that single source line to disable.

# Enable syntax highlighting + autosuggestions if the packages are installed
[ -f /usr/share/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh ] && \
    source /usr/share/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh
[ -f /usr/share/zsh-autosuggestions/zsh-autosuggestions.zsh ] && \
    source /usr/share/zsh-autosuggestions/zsh-autosuggestions.zsh
ZSH_AUTOSUGGEST_HIGHLIGHT_STYLE='fg=#3a5a70'

# Quality-of-life aliases (only if the tool exists)
command -v eza >/dev/null 2>&1 && {
    alias ls='eza --group-directories-first --icons=auto'
    alias ll='eza -lah --group-directories-first --icons=auto --git'
    alias lt='eza --tree --level=2 --icons=auto'
}
command -v batcat >/dev/null 2>&1 && alias cat='batcat --style=plain --paging=never'
command -v bat    >/dev/null 2>&1 && alias cat='bat --style=plain --paging=never'
alias ports='sudo ss -tulpn'
alias myip='ip -br addr | grep -v "^lo"'
alias please='sudo $(fc -ln -1)'
alias matrix='cmatrix -b -C cyan'
alias hud='conky -c ~/.config/conky/alisha.conf >/dev/null 2>&1 &!'

# ALISHA greeting on a new interactive shell
if [[ $- == *i* ]] && [[ -z "$ALISHA_GREETED" ]]; then
    export ALISHA_GREETED=1
    bash ~/.config/alisha/greeting.sh
fi
