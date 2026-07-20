#include "mainCore.hxx"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <link.h>
#include "xdl.h"
#include "log.h"
#include <iostream>

#define LOGCXX(x) std::cout << x << std::endl

void mainCore(const char* Bname) {
    LOGCXX(Bname);
    capcap(Bname);
}

void capcap(const char* mName) {
    if (mName) {
    void *handle = xdl_open(mName, XDL_DEFAULT);
    if (handle) {
        xdl_info_t info;
        if (xdl_info(handle, XDL_DI_DLINFO, &info) == 0) {

         //   LOGI("hooks for %s: %s", mName, info.dli_fname);

            int fd = open(info.dli_fname, O_RDONLY);
            if (fd >= 0) {
                for (size_t i = 0; i < info.dlpi_phnum; i++) {
                  const ElfW(Phdr) *phdr = &info.dlpi_phdr[i];
                    if (phdr->p_type == PT_LOAD && (phdr->p_flags & PF_X)) {
                        uintptr_t addr = (uintptr_t) info.dli_fbase + phdr->p_vaddr;
                        size_t __tZe = phdr->p_filesz;
                         long mPage = sysconf(_SC_PAGESIZE);
                          uintptr_t bPage = addr & ~(mPage - 1);
                           size_t mSize = (addr + __tZe - bPage + mPage - 1) & ~(mPage - 1);
                            if (mprotect((void *) bPage, mSize,PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                            /**
                             * @addtogroup
                             * lseek seek pass
                             * read pass
                             * mprotect pass
                             */
                            lseek(fd, phdr->p_offset, SEEK_SET);
                            read(fd, (void *) addr, __tZe);
                            mprotect((void *) bPage, mSize, PROT_READ | PROT_EXEC);
                        }
                      }
                    }
                close(fd);
             }

           }

         xdl_close(handle);

       } else {

        LOGE("failed to open %s", mName);

    }
   return;
  }
}
