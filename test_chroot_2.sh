mkdir -p /tmp/jail-test/{bin,lib64,usr,etc}
sudo mount --make-private /
sudo mount --bind /bin /tmp/jail-test/bin
sudo mount --bind /lib64 /tmp/jail-test/lib64
sudo mount --bind /usr /tmp/jail-test/usr
mkdir -p /tmp/jail-test/home/samwasted/codecrafters-shell-cpp/src
sudo mount --bind $(pwd) /tmp/jail-test/home/samwasted/codecrafters-shell-cpp/src
sudo chroot /tmp/jail-test /bin/sh -c 'ls -la /home/samwasted/codecrafters-shell-cpp/src/the_hi'
sudo chroot /tmp/jail-test /bin/sh -c '/home/samwasted/codecrafters-shell-cpp/src/the_hi'
sudo umount /tmp/jail-test/bin /tmp/jail-test/lib64 /tmp/jail-test/usr /tmp/jail-test/home/samwasted/codecrafters-shell-cpp/src
rm -rf /tmp/jail-test
