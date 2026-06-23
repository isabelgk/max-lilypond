{
    "patcher": {
        "fileversion": 1,
        "appversion": {
            "major": 9,
            "minor": 3,
            "revision": 0,
            "architecture": "x64",
            "modernui": 1
        },
        "classnamespace": "box",
        "rect": [ 134.0, 172.0, 640.0, 520.0 ],
        "showrootpatcherontab": 0,
        "showontab": 0,
        "boxes": [
            {
                "box": {
                    "id": "obj-2",
                    "maxclass": "newobj",
                    "numinlets": 0,
                    "numoutlets": 0,
                    "patcher": {
                        "fileversion": 1,
                        "appversion": {
                            "major": 9,
                            "minor": 3,
                            "revision": 0,
                            "architecture": "x64",
                            "modernui": 1
                        },
                        "classnamespace": "box",
                        "rect": [ 0.0, 26.0, 640.0, 494.0 ],
                        "showontab": 1,
                        "boxes": [],
                        "lines": []
                    },
                    "patching_rect": [ 75.0, 117.0, 25.0, 22.0 ],
                    "text": "p ?"
                }
            },
            {
                "box": {
                    "id": "obj-1",
                    "maxclass": "newobj",
                    "numinlets": 0,
                    "numoutlets": 0,
                    "patcher": {
                        "fileversion": 1,
                        "appversion": {
                            "major": 9,
                            "minor": 3,
                            "revision": 0,
                            "architecture": "x64",
                            "modernui": 1
                        },
                        "classnamespace": "box",
                        "rect": [ 134.0, 198.0, 640.0, 494.0 ],
                        "showontab": 1,
                        "boxes": [
                            {
                                "box": {
                                    "attr": "size",
                                    "id": "obj-5",
                                    "maxclass": "attrui",
                                    "numinlets": 1,
                                    "numoutlets": 1,
                                    "outlettype": [ "" ],
                                    "parameter_enable": 0,
                                    "patching_rect": [ 226.0, 230.0, 150.0, 22.0 ]
                                }
                            },
                            {
                                "box": {
                                    "attr": "margin",
                                    "id": "obj-4",
                                    "maxclass": "attrui",
                                    "numinlets": 1,
                                    "numoutlets": 1,
                                    "outlettype": [ "" ],
                                    "parameter_enable": 0,
                                    "patching_rect": [ 226.0, 256.0, 150.0, 22.0 ]
                                }
                            },
                            {
                                "box": {
                                    "border": 0,
                                    "filename": "helpdetails.js",
                                    "id": "obj-2",
                                    "ignoreclick": 1,
                                    "jsarguments": [ "lilypond" ],
                                    "maxclass": "jsui",
                                    "numinlets": 1,
                                    "numoutlets": 1,
                                    "outlettype": [ "" ],
                                    "parameter_enable": 0,
                                    "patching_rect": [ 13.0, 14.0, 600.0, 120.0 ]
                                }
                            },
                            {
                                "box": {
                                    "id": "obj-set1",
                                    "maxclass": "message",
                                    "numinlets": 2,
                                    "numoutlets": 1,
                                    "outlettype": [ "" ],
                                    "patching_rect": [ 23.0, 162.0, 97.0, 22.0 ],
                                    "text": "set { c' d' e' f' g' }"
                                }
                            },
                            {
                                "box": {
                                    "id": "obj-set2",
                                    "maxclass": "message",
                                    "numinlets": 2,
                                    "numoutlets": 1,
                                    "outlettype": [ "" ],
                                    "patching_rect": [ 42.0, 204.0, 128.0, 22.0 ],
                                    "text": "set { c'4 e'8 g' c''2 g'4 }"
                                }
                            },
                            {
                                "box": {
                                    "autofind": 1,
                                    "color": [ 1.0, 1.0, 1.0, 1.0 ],
                                    "id": "obj-lily",
                                    "lilypondpath": "/opt/homebrew/bin/lilypond",
                                    "ly_source": "{ c' d' e' f' g' }",
                                    "margin": 14.0,
                                    "maxclass": "lilypond",
                                    "numinlets": 1,
                                    "numoutlets": 0,
                                    "patching_rect": [ 23.0, 308.0, 202.0, 81.0 ],
                                    "size": 8.0
                                }
                            }
                        ],
                        "lines": [
                            {
                                "patchline": {
                                    "destination": [ "obj-lily", 0 ],
                                    "source": [ "obj-5", 0 ]
                                }
                            },
                            {
                                "patchline": {
                                    "destination": [ "obj-lily", 0 ],
                                    "source": [ "obj-4", 0 ]
                                }
                            },
                            {
                                "patchline": {
                                    "destination": [ "obj-lily", 0 ],
                                    "source": [ "obj-set1", 0 ]
                                }
                            },
                            {
                                "patchline": {
                                    "destination": [ "obj-lily", 0 ],
                                    "source": [ "obj-set2", 0 ]
                                }
                            }
                        ]
                    },
                    "patching_rect": [ 26.0, 50.0, 47.0, 22.0 ],
                    "text": "p basic"
                }
            }
        ],
        "lines": [],
        "autosave": 0
    }
}