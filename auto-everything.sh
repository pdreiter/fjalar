cd vex && make clean all
cd ../valgrind
./autogen.sh && ./configure --prefix=`pwd`/inst --with-vex=`pwd`/../vex && make && make install