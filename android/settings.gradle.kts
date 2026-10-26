// «Компьютер рядом» - приложение к «Телефону рядом» из RadialMenu (src/phone). Сборка: build.cmd в этой папке.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "MoonNear"
include(":app")
