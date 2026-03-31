mkdir -p /tmp/jail-test/{bin,lib64,usr}
sudo mount --make-private /
sudo mount --bind /bin /tmp/jail-test/bin
sudo mount --bind /lib64 /tmp/jail-test/lib64
sudo mount --bind /usr /tmp/jail-test/usr
sudo mount --bind . /tmp/jail-test/.
sudo chroot /tmp/jail-test /home/samwasted/codecrafters-shell-cpp/src/the_hi
sudo umount /tmp/jail-test/. /tmp/jail-test/bin /tmp/jail-test/lib64 /tmp/jail-test/usr
rm -rf /tmp/jail-test
