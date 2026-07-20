import android.databinding.tool.ext.capitalizeUS
import org.apache.tools.ant.filters.FixCrLfFilter
import org.apache.tools.ant.filters.ReplaceTokens
import java.security.MessageDigest

plugins {
    alias(libs.plugins.agp.app)
}

val moduleId: String by rootProject.extra
val moduleName: String by rootProject.extra
val verCode: Int by rootProject.extra
val verName: String by rootProject.extra

val abiList: List<String> by rootProject.extra

android {
    defaultConfig {
        ndk {
            abiFilters.addAll(abiList)
        }
        externalNativeBuild {

            cmake {
                cppFlags("-std=c++20")
                arguments(
                    "-DANDROID_STL=c++_static",
                    "-DMODULE_NAME=$moduleId"
                )
            }
        }
    }
    externalNativeBuild {
        ndkBuild {
            path("src/main/cpp/Android.mk")
        }
    }
}

tasks.register<Exec>("ArtRestart") {
    group = "module"
    description = "Restart ART (zygote) safely without full reboot"
    isIgnoreExitValue = true
    val devices = try {
    "adb devices".execute().trim().lines().drop(1).filter { it.isNotBlank() }
    } catch (e: Exception) {
    emptyList()
    }
    if (devices.isNotEmpty()) {
        val serial = devices.first().split("\\s+".toRegex())[0]
        commandLine(
            "adb", "-s", serial, "shell", "su", "-c",
            """
            killall zygote
            killall zygote64
            start zygote
            start zygote64
            exit 0
            """.trimIndent()
        )
    } else {
        commandLine("adb", "devices")
    }

    onlyIf { devices.isNotEmpty() }
}

fun String.execute(): String {
    val process = Runtime.getRuntime().exec(this)
    process.waitFor()
    return process.inputStream.bufferedReader().readText()
}

androidComponents.onVariants { variant ->
    afterEvaluate {
        val variantLowered = variant.name.lowercase()
        val variantCapped = variant.name.capitalizeUS()
        val buildTypeLowered = variant.buildType?.lowercase()
        val supportedAbis = abiList.joinToString(" ") {
            when (it) {
                "arm64-v8a" -> "arm64"
                "armeabi-v7a" -> "arm"
                "x86" -> "x86"
                "x86_64" -> "x64"
                else -> error("unsupported abi $it")
            }
        }

        val moduleDir = layout.buildDirectory.file("outputs/module/$variantLowered")
        val zipFileName =
            "$moduleName-$verName-$verCode-$buildTypeLowered.zip".replace(' ', '-')

        val prepareModuleFilesTask = task<Sync>("prepareModuleFiles$variantCapped") {
            group = "module"
            dependsOn("assemble$variantCapped")
            into(moduleDir)
            from(layout.projectDirectory.file("template")) {
                exclude("module.prop", "customize.sh", "post-fs-data.sh", "service.sh")
                filter<FixCrLfFilter>("eol" to FixCrLfFilter.CrLf.newInstance("lf"))
            }
            from(layout.projectDirectory.file("template")) {
                include("module.prop")
                expand(
                    "moduleId" to moduleId,
                    "moduleName" to moduleName,
                    "versionName" to verName,
                    "versionCode" to verCode,
                    "description" to rootProject.extra["description"],
                )
            }
            from(layout.projectDirectory.file("template")) {
                include("customize.sh", "post-fs-data.sh", "service.sh")
                val tokens = mapOf(
                    "DEBUG" to if (buildTypeLowered == "debug") "true" else "false",
                    "SONAME" to moduleId,
                    "SUPPORTED_ABIS" to supportedAbis
                )
                filter<ReplaceTokens>("tokens" to tokens)
                filter<FixCrLfFilter>("eol" to FixCrLfFilter.CrLf.newInstance("lf"))
            }
            from(layout.buildDirectory.file("intermediates/stripped_native_libs/$variantLowered/strip${variantCapped}DebugSymbols/out/lib")) {
                into("lib")
            }

            doLast {
                fileTree(moduleDir).visit {
                    if (isDirectory) return@visit
                    val md = MessageDigest.getInstance("SHA-256")
                    file.forEachBlock(4096) { bytes, size ->
                        md.update(bytes, 0, size)
                    }
                    file(file.path + ".sha256").writeText(
                        org.apache.commons.codec.binary.Hex.encodeHexString(
                            md.digest()
                        )
                    )
                }
            }
        }


        val zipTask = task<Zip>("zip$variantCapped") {
            group = "module"
            dependsOn(prepareModuleFilesTask)
            archiveFileName.set(zipFileName)
            destinationDirectory.set(layout.projectDirectory.file("release").asFile)
            from(moduleDir)
        }

        val devices = try {
            "adb devices".execute().trim().lines().drop(1).filter { it.isNotBlank() }
        } catch (e: Exception) {
            emptyList()
        }
        val adbBase = if (devices.isNotEmpty()) {
            listOf("adb", "-s", devices.first().split("\\s+".toRegex())[0])
        } else {
            listOf("adb")
        }

        val pushTask = task<Exec>("push$variantCapped") {
            group = "module"
            dependsOn(zipTask)
            commandLine(adbBase + listOf("push", zipTask.outputs.files.singleFile.path, "/data/local/tmp"))
            onlyIf { devices.isNotEmpty() }
        }

        val installKsuTask = task<Exec>("installKsu$variantCapped") {
            group = "module"
            dependsOn(pushTask)
            commandLine(
                adbBase + listOf(
                    "shell", "su", "-c",
                    "/data/adb/ksud module install /data/local/tmp/$zipFileName"
                )
            )
            onlyIf { devices.isNotEmpty() }
        }

        val installMagiskTask = task<Exec>("installMagisk$variantCapped") {
            group = "module"
            dependsOn(pushTask)
            commandLine(
                adbBase + listOf(
                    "shell",
                    "su",
                    "-M",
                    "-c",
                    "magisk --install-module /data/local/tmp/$zipFileName"
                )
            )
            onlyIf { devices.isNotEmpty() }
        }
    }
}
