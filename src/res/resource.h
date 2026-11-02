// Resource IDs other than commands (see ui/Commands.h).
#pragma once

#define IDI_APP                     100
#define IDR_MAINMENU                101
#define IDR_CONTEXTMENU             102
#define IDD_PROPERTIES              110
#define IDC_PROPERTIES_LIST         1001
#ifndef IDC_STATIC
#define IDC_STATIC                  (-1)
#endif
#define IDD_BATCH_RENAME            111
#define IDC_BR_PATTERN              1010
#define IDC_BR_START                1011
#define IDC_BR_LOWER_EXT            1012
#define IDC_BR_LIST                 1013
#define IDC_BR_STATUS               1014
#define IDC_BR_PROGRESS             1015

// ---- String table -----------------------------------------------------------
#define IDS_APP_TITLE               1000
#define IDS_ABOUT_TEXT              1001
#define IDS_STATUS_READY            1002
#define IDS_NOT_IMPLEMENTED         1003
#define IDS_ERR_STARTUP             1004
#define IDS_ERR_SETTINGS_READ       1005
#define IDS_ERR_SETTINGS_WRITE      1006

// Browser (T4)
#define IDS_COL_NAME                1100
#define IDS_COL_SIZE                1101
#define IDS_COL_TYPE                1102
#define IDS_COL_MODIFIED            1103
#define IDS_TREE_LOADING            1110
#define IDS_STATUS_LOADING          1120
#define IDS_STATUS_FILES            1121
#define IDS_STATUS_SELECTED         1122
#define IDS_ERR_OPEN_FOLDER         1123

// Viewer (T6)
#define IDS_VIEWER_ERROR            1200
#define IDS_VIEWER_INDEX            1201
#define IDS_VIEWER_ZOOM             1202
#define IDS_VIEWER_LOADING          1203

// File management (T7)
#define IDS_NEW_FOLDER              1300
#define IDS_PICK_COPY_TARGET        1301
#define IDS_PICK_MOVE_TARGET        1302
#define IDS_ERR_RENAME              1303
#define IDS_ERR_INVALID_NAME        1304
#define IDS_ERR_ROTATE              1305
#define IDS_ERR_FILE_OP             1306
#define IDS_STATUS_ROTATING         1307
#define IDS_PROP_FIELD              1310
#define IDS_PROP_VALUE              1311
#define IDS_PROP_NAME               1312
#define IDS_PROP_FOLDER             1313
#define IDS_PROP_SIZE               1314
#define IDS_PROP_DIMENSIONS         1315
#define IDS_PROP_MODIFIED           1316
#define IDS_PROP_TAKEN              1317
#define IDS_PROP_CAMERA             1318
#define IDS_PROP_EXPOSURE           1319
#define IDS_PROP_APERTURE           1320
#define IDS_PROP_ISO                1321
#define IDS_PROP_FOCAL              1322
#define IDS_PROP_TITLE              1323

// Batch rename (T8)
#define IDS_BR_COL_FROM             1400
#define IDS_BR_COL_TO               1401
#define IDS_BR_COL_ISSUE            1402
#define IDS_BR_ISSUE_DUPLICATE      1403
#define IDS_BR_ISSUE_INVALID        1404
#define IDS_BR_ISSUE_TOO_LONG       1405
#define IDS_BR_ISSUE_EXISTS         1406
#define IDS_BR_CONFLICTS            1407
#define IDS_BR_READY                1408
#define IDS_ERR_BATCH_RENAME        1409
#define IDS_ERR_UNDO_RENAME         1410
#define IDS_BR_NOTHING_TO_UNDO      1411

// Toolbar tooltips use the command ID as the string ID.

// Command bar labels
#define IDS_BAR_VIEW_PICTURE        1500
#define IDS_BAR_BROWSE              1501
#define IDS_BAR_SORT                1502
#define IDS_BAR_VIEW                1503

// Image format extensions (codec packs)
#define IDS_STATUS_CODEC_HINT       1600
#define IDS_CODECS_ALL_INSTALLED    1601
#define IDS_CODECS_MISSING          1602
#define IDS_CODECS_TITLE            1603
#define IDS_VIEWER_ERROR_HEIF       1604
#define IDS_LIST_SEPARATOR          1605

// Text files
#define IDS_TEXT_ERROR              1700
#define IDS_TEXT_TRUNCATED          1701

// PDF
#define IDS_PDF_PAGE                1800

// Documents (preview handlers)
#define IDS_DOC_PREVIEW             1900
#define IDS_DOC_ERROR               1901
#define IDS_DOC_NO_HANDLER          1902
#define IDS_DOC_NO_HANDLER_SHORT    1903

// 3D models
#define IDS_MODEL_ERROR             2000
#define IDS_MODEL_TRIANGLES         2001

// Videos
#define IDS_VIDEO_ERROR             2100
#define IDS_VIDEO_NO_CODEC          2101
#define IDS_VIDEO_NO_CODEC_SHORT    2102
#define IDS_VIDEO_PLAYING           2103
#define IDS_VIDEO_PAUSED            2104
#define IDS_VIDEO_ENDED             2105

// Preview pane
#define IDS_PREVIEW_EMPTY           2200
#define IDS_PREVIEW_NONE            2201
