#!/bin/sh
# Сетевой интерфейс хоста для сетевой карты AZБК (--az-net, меню «Контроллер AZБК» → «Сеть»).
# Создаётся один раз через sudo и отдаётся пользователю — эмулятору права не нужны.
#
#   azbk-net.sh macvtap <карта> [имя] [AZ.INI]
#       под-интерфейс macvtap на физической карте: у платы свой MAC в локальной сети и
#       адрес от её DHCP, как у настоящей. MAC берётся из MAC= в [NETWORK] файла AZ.INI
#       (по умолчанию — roms/azbk/AZ.INI рядом со скриптом). Эмулятор: --az-net macvtap:<имя>
#   azbk-net.sh tap [имя]
#       постоянный tap-интерфейс (дальше — мост или NAT вручную). Эмулятор: --az-net tap:<имя>
#   azbk-net.sh remove [имя]
#
# Хост сам до платы через macvtap не достучится (так устроен macvlan): для этого нужен
# второй macvlan-интерфейс на той же карте у хоста.
set -e
here=$(cd "$(dirname "$0")" && pwd)
cmd=$1
case "$cmd" in
macvtap)
    parent=$2
    name=${3:-azbk0}
    ini=${4:-$here/../roms/azbk/AZ.INI}
    [ -n "$parent" ] || { echo "нужна физическая карта: $0 macvtap enp3s0" >&2; exit 1; }
    hex=$(sed -n 's/\r//; /^\[NETWORK\]/,/^\[/{s/^MAC=\([0-9A-Fa-f]\{12\}\)$/\1/p}' "$ini" | head -n1)
    [ -n "$hex" ] || hex=000E35220B01             # MAC прошивки по умолчанию
    mac=$(echo "$hex" | sed 's/../&:/g; s/:$//' | tr 'A-F' 'a-f')
    sudo ip link add link "$parent" name "$name" type macvtap mode bridge
    sudo ip link set "$name" address "$mac" up
    dev=/dev/tap$(cat "/sys/class/net/$name/ifindex")
    sudo chown "$(id -u)" "$dev"
    echo "готово: $name ($mac) на $parent, устройство $dev — запуск: --az-net macvtap:$name"
    ;;
tap)
    name=${2:-azbktap0}
    sudo ip tuntap add "$name" mode tap user "$(id -un)"
    sudo ip link set "$name" up
    echo "готово: $name — запуск: --az-net tap:$name (подключите его к мосту или NAT)"
    ;;
remove)
    name=${2:-azbk0}
    sudo ip link del "$name"
    ;;
*)
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
