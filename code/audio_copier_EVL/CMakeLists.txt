cmake_minimum_required(VERSION 3.28)

project(AudioCopierEVL C)

add_executable(audio_copier_EVL
               audio_copier_EVL.c
               ../de1soc_io/de1soc_io.c
)

target_include_directories(audio_copier_EVL PRIVATE
                           ../de1soc_io
)

target_link_libraries(audio_copier_EVL PRIVATE evl pthread)