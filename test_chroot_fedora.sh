mkdir -p /tmp/jail-test
mkdir -p /tmp/jail-test/usr
sudo mount --make-private /
sudo mount --bind /usr /tmp/jail-test/usr
ln -s usr/bin /tmp/jail-test/bin
ln -s usr/sbin /tmp/jail-test/sbin
ln -s usr/lib /tmp/jail-test/lib
ln -s usr/lib64 /tmp/jail-test/lib64
mkdir -p /tmp/jail-test/home/samwasted/codecrafters-shell-cpp/src
sudo mount --bind $(pwd) /tmp/jail-test/home/samwasted/codecrafters-shell-cpp/src
sudo chroot /tmp/jail-test /home/samwasted/codecrafters-shell-cpp/src/the_hi
sudo umount /tmp/jail-test/usr /tmp/jail-test/home/samwasted/codecrafters-shell-cpp/src
rm -rf /tmp/jail-test
