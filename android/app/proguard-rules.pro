# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
#
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# Keep native methods
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep the NativeEmulator class and all its methods
-keep class com.x360mu.core.NativeEmulator { *; }
-keep class com.x360mu.core.NativeEmulator$* { *; }

# Keep VibrationManager — JNI reverse callback uses onVibration(III)V by name
-keep class com.x360mu.core.VibrationManager { *; }
-keep class com.x360mu.core.VibrationManager$VibrationListener { *; }

# Keep Compose
-keep class androidx.compose.** { *; }

# Keep reflection for Kotlin
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.AnnotationsKt

-keepclassmembers class kotlinx.serialization.json.** {
    *** Companion;
}
-keepclasseswithmembers class kotlinx.serialization.json.** {
    kotlinx.serialization.KSerializer serializer(...);
}

# Uncomment this to preserve the line number information for
# debugging stack traces.
-keepattributes SourceFile,LineNumberTable

# If you keep the line number information, uncomment this to
# hide the original source file name.
#-renamesourcefileattribute SourceFile

