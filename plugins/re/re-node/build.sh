V8=~/git/v8
g++ -fPIC -shared -std=c++1y -I$V8 libre-v8.cpp -o libre-v8 -Wl,--start-group $V8/out/x64.release/obj.target/{tools/gyp/libv8_{base,libbase,snapshot,libplatform},third_party/icu/libicu{uc,i18n,data}}.a -Wl,--end-group -lrt -ldl -pthread -g
