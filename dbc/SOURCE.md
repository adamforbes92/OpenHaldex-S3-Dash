# DBC files

`vw_pq.dbc` and `vw_mqb.dbc` come from https://github.com/commaai/opendbc.git at commit
`35f7e0813462607ef1d703e52313e7571e31405d 2026-10-02`.

Licence: MIT (Copyright (c) 2020, Comma.ai, Inc.), see LICENSE in this folder.

The message and signal names have been translated from German to English by Forbes
Automotive; the ids, bit positions and scaling are unchanged, and each renamed item keeps
its German name in a comment (`[DE: ...]`), so the files still match VW K-matrix spreadsheets.

Run `python tools/make_dbc_tables.py` after changing anything here: it regenerates
`src/net/dbc_tables.cpp`, which is what the firmware actually uses.
