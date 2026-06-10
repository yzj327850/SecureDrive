#include "random.h"
#include <cstring>

#ifdef _WIN32
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

bool secure_random(uint8_t* buf, size_t len) {
    if(!buf || len==0) return false;

#ifdef _WIN32
    NTSTATUS status = BCryptGenRandom(
        nullptr, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(status);

#else
    // macOS / Linux: 读 /dev/urandom（非阻塞，足够安全）
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if(fd < 0) return false;
    size_t done = 0;
    while(done < len){
        ssize_t n = read(fd, buf+done, len-done);
        if(n <= 0){ close(fd); return false; }
        done += (size_t)n;
    }
    close(fd);
    return true;
#endif
}
