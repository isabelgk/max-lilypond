set_property(DIRECTORY PROPERTY max::external YES)
project(lilypond)

target_sources(lilypond
    PRIVATE
    lilypond.c
)
