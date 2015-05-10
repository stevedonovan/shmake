rm /tmp/shmake
touch /tmp/shmake
pipe() {
    res=''
    for f in "$@"; do res="$res:$f"; done
    echo $res >> /tmp/shmake
}
C() { pipe C "$@"; }
T() { pipe target "$@"; }
all() { pipe all "$@"; }

