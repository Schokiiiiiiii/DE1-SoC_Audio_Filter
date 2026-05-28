cmake_minimum_required(VERSION 3.28)

project(AudioCopier C)

add_executable(audio_copier
        audio_copier.c
)

target_include_directories(audio_copier PRIVATE
        ../../
)

target_link_libraries(audio_copier PRIVATE
        pthread
)