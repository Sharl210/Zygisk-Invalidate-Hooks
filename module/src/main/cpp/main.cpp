#include <jni.h>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include "zygisk.hpp"
#include "mainCore.hxx"
#include "log.h"

/**
 *
 * @@@@@@@@ Antik Mods @@@@@@@
 *
 *
 * Start -> Read Config -> Check Package -> mainCore (Restoration)
 *                                               |
 *                                               v
 * [Disk File] ----(Read Clean Bytes)----> [Memory Address]
 *                                               |
 * permissions:  mprotect(RWX) -> Copy -> mprotect(RX)
 *
 */

using namespace zygisk;
using namespace std;

#define linName "libart.so"

struct Config {
    bool enabled = false;
    string mName = "libart.so";
    vector<string> packages;
}
config;

/**
 * @addtogroup
 * @return
 */

static Config _Read()

{

    ifstream file("/data/adb/modules/inline_hook_spoof/config.txt");

    if (file.is_open())

    {
        string mLine;

        if (getline(file, mLine))

        {
            size_t pos = mLine.find(':');
            if (pos != string::npos)

            {
                config.enabled = (mLine.substr(0, pos) == "1");
                config.mName = mLine.substr(pos + 1).erase(mLine.substr(pos + 1).find_last_not_of("\r\n") + 1);
                /**
                 *
                 * if my config send null file then i can use this libart.so defult
                 */
                if (config.mName.empty()) config.mName = linName;

            }

        }

        while (getline(file, mLine))

        {
            mLine.erase(mLine.find_last_not_of("\r\n") + 1);

            if (!mLine.empty())

            {
                config.packages.push_back(mLine);
            }

        }

        file.close();
    } else {
        // LOGE("Could not open config file /data/adb/modules/inline_hook_spoof/config.txt");
    }
    return config;
}

class MyModule : public ModuleBase {

private:
    bool mFound = false;

public:

    JavaVM* cMov = nullptr;

    void onLoad(Api* api, JNIEnv* env) override {
        env->GetJavaVM(&cMov);
    }

    /**
     *  void (*preAppSpecialize)(ModuleBase *, AppSpecializeArgs *);
            void (*postAppSpecialize)(ModuleBase *, const AppSpecializeArgs *);
            void (*preServerSpecialize)(ModuleBase *, ServerSpecializeArgs *);
            void (*postServerSpecialize)(ModuleBase *, const ServerSpecializeArgs *);
     * @param args
     */

    void preAppSpecialize(AppSpecializeArgs* nAme) override {

        if (nAme && nAme->nice_name) {

            JNIEnv *env = nullptr;

            if (cMov->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) {
                cMov->AttachCurrentThread(&env, nullptr);
            }

            if (env) {

                const char *proc = env->GetStringUTFChars(nAme->nice_name, nullptr);

                if (proc) {

                    // object from config class ok dude

                    Config config = _Read();

                    if (config.enabled) {

                        int allpkg = config.packages.size();

                          for (int i = 0; i < allpkg; i++) {

                            string pkg = config.packages[i];

                            // LOGD("PKG --> %s", pkg);

                            if (strcmp(proc, pkg.c_str()) == 0) {
                                mFound = true;
                                break;
                            }

                           }
                           if (mFound) {

                            // LOGI("cleaning %s in %s", config.mName.c_str(), proc);

                            /**
                             * @aantik_mods
                             * @Main Core Call
                             */
                            mainCore(config.mName.c_str());

                           }

                         }

                       env->ReleaseStringUTFChars(nAme->nice_name, proc);

                     }

                   }
                // cao bana
             }
         return;
      }
    //
};

REGISTER_ZYGISK_MODULE(MyModule)
