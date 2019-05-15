AC_DEFUN([CC_CHECK_DROID_ENUM],
[AC_MSG_CHECKING([if droid headers have enum $2])
 AC_LANG_SAVE
 AC_LANG_C
 SAVE_CFLAGS="$CFLAGS"
 CFLAGS="$CFLAGS $1"
 AC_TRY_COMPILE(
 [ #include <android-config.h>
   #ifdef QCOM_BSP
   #define QCOM_HARDWARE
   #endif
   #include <system/audio.h> ],
 [ unsigned int e = $2; ],
 cc_check_droid_enum=yes, cc_check_droid_enum=no)
 CFLAGS="$SAVE_CFLAGS"
 AC_LANG_RESTORE
 AC_MSG_RESULT([$cc_check_droid_enum])
if test x"$cc_check_droid_enum" = x"yes"; then
  AC_DEFINE(HAVE_ENUM_$2,,[define if enum $2 is found in headers])
  AC_DEFINE(STRING_ENTRY_IF_$2,[STRING_ENTRY($2),],[string entry for enum $2])
  AC_DEFINE(FANCY_ENTRY_IF_$2(n),[{$2, n},],[fancy entry for enum $2])
else
  AC_DEFINE(STRING_ENTRY_IF_$2,,[string entry for enum $2])
  AC_DEFINE(FANCY_ENTRY_IF_$2(n),,[fancy entry for enum $2])
fi
])
