dnl
dnl check where to install documentation
dnl
dnl determines documentation "root directory", i.e. the directory
dnl where all documentation will be placed in
dnl

AC_DEFUN(GP_CHECK_DOC_DIR,
[
AC_BEFORE([$0], [GP_BUILD_DOCS])dnl

AC_ARG_WITH(doc-dir, [  --with-doc-dir=PATH       Where to install docs  [default=autodetect]])dnl

# check for the main ("root") documentation directory
AC_MSG_CHECKING([main docdir])

if test "x${with_doc_dir}" != "x"
then # docdir is given as parameter
    DOC_DIR="${with_doc_dir}"
    AC_MSG_RESULT([${DOC_DIR} (from parameter)])
else # otherwise invent a docdir hopefully compatible with system policy
    if test -d "/usr/share/doc"
    then
        maindocdir='${prefix}/share/doc'
        AC_MSG_RESULT([${maindocdir} (FHS style)])
    elif test -d "/usr/doc"
    then
        maindocdir='${prefix}/doc'
        AC_MSG_RESULT([${maindocdir} (old style)])
    else
        maindocdir='${datadir}/doc'
        AC_MSG_RESULT([${maindocdir} (default value)])
    fi
    AC_MSG_CHECKING(package docdir)
    # check whether to include package version into documentation path
    if ls -d /usr/{share/,}doc/*-[[]0-9[]]* > /dev/null 2>&1
    then
        DOC_DIR="${maindocdir}/${PACKAGE}-${VERSION}"
        AC_MSG_RESULT([${DOC_DIR} (redhat style)])
    else
        DOC_DIR="${maindocdir}/${PACKAGE}"
        AC_MSG_RESULT([${DOC_DIR} (default style)])
    fi
fi

AC_SUBST(DOC_DIR)

])dnl

dnl
dnl check whether to build docs and where to:
dnl
dnl * determine presence of prerequisites (only gtk-doc for now)
dnl * determine destination directory for HTML files
dnl

AC_DEFUN(GP_BUILD_DOCS,
[
# doc dir has to be determined in advance
AC_REQUIRE([GP_CHECK_DOC_DIR])

dnl ---------------------------------------------------------------------------
dnl fig2dev: This program is needed for processing images. If not found,
dnl          documentation can still be built, but without figures.
dnl ---------------------------------------------------------------------------
try_fig2dev=true
have_fig2dev=false
AC_ARG_WITH(fig2dev, [  --without-fig2dev         Don't use fig2dev],[
	if test "x$withval" = "xno"; then
		try_fig2dev=false
	fi])
if $try_fig2dev; then
	AC_PATH_PROG(FIG2DEV,fig2dev)
	if test -n "${FIG2DEV}"; then
		have_fig2dev=true
	fi
fi
if $have_fig2dev; then
        ${FIG2DEV} -L ps > /dev/null <<EOF
#FIG 3.2
Landscape
Center
Inches
Letter  
100.00
Single
-2
1200 2
1 3 0 1 0 7 50 0 -1 0.000 1 0.0000 3000 3750 270 270 3000 3750 3150 3975
EOF
        if test $? != 0; then
                have_fig2dev=false
        fi
fi
AM_CONDITIONAL(ENABLE_FIGURES, $have_fig2dev)

dnl ---------------------------------------------------------------------------
dnl gtk-doc: We use gtk-doc for building our documentation. However, we
dnl          require the user to explicitely request the build.
dnl ---------------------------------------------------------------------------
try_gtkdoc=false
gtkdoc_msg="no (not requested)"
have_gtkdoc=false
AC_ARG_ENABLE(docs, [  --enable-docs             Use gtk-doc to build documentation [default=no]],[
	if test x$enableval = xyes; then
		try_gtkdoc=true
	fi])
if $try_gtkdoc; then
	AC_PATH_PROG(GTKDOC,gtkdoc-mkdb)
	if test -n "${GTKDOC}"; then
		have_gtkdoc=true
		gtkdoc_msg="yes"
	else
		gtkdoc_msg="no (http://www.gtk.org/rdp/download.html)"
	fi
fi
AM_CONDITIONAL(ENABLE_GTK_DOC, $have_gtkdoc)

dnl ---------------------------------------------------------------------------
dnl Give the user the possibility to install html documentation in a 
dnl user-defined location.
dnl ---------------------------------------------------------------------------
AC_ARG_WITH(html-dir, [  --with-html-dir=PATH      Where to install html docs [default=autodetect]])
AC_MSG_CHECKING([for html dir])
if test "x${with_html_dir}" = "x" ; then
    HTML_DIR="${DOC_DIR}/html"
    AC_MSG_RESULT([${HTML_DIR} (default)])
else
    HTML_DIR="${with_html_dir}"
    AC_MSG_RESULT([${HTML_DIR} (from parameter)])
fi
AC_SUBST(HTML_DIR)
API_DIR="${HTML_DIR}/api"
AC_SUBST(API_DIR)

dnl ------------------------------------------------------------------------
dnl try to find xmlto (required for generation of man pages and html docs)
dnl ------------------------------------------------------------------------
manual_msg="no (http://cyberelk.net/tim/xmlto/)"
try_xmlto=true
have_xmlto=false
have_xmltohtml=false
have_xmltoman=false
have_xmltopdf=false
have_xmltops=false
AC_ARG_WITH(xmlto, [  --without-xmlto           Don't use xmlto],[
	if test x$withval = xno; then
		try_xmlto=false
	fi])
if $try_xmlto; then
	AC_PATH_PROG(XMLTO,xmlto)
	if test -n "${XMLTO}"; then
		have_xmlto=true
		manual_msg="yes"
	fi
fi
if $have_xmlto; then
	if ${XMLTO} --help | grep html > /dev/null 2>&1; then
		have_xmltohtml=true
	fi
	if ${XMLTO} --help | grep man > /dev/null 2>&1; then
		have_xmltoman=true
	fi
	if ${XMLTO} --help | grep pdf > /dev/null 2>&1; then
		have_xmltopdf=true
	fi
	if ${XMLTO} --help | grep ps > /dev/null 2>&1; then
		have_xmltops=true
	fi
fi
AM_CONDITIONAL(XMLTO, $have_xmlto)
AM_CONDITIONAL(XMLTOHTML,$have_xmltohtml)
AM_CONDITIONAL(XMLTOMAN,$have_xmltoman)
AM_CONDITIONAL(XMLTOPDF,$have_xmltopdf)
AM_CONDITIONAL(XMLTOPS,$have_xmltops)

# create list of supported formats
xxx=""
manual_html=""
manual_pdf=""
manual_ps=""
if test "x$XMLTOHTML_FALSE" = "x#"; then
        xxx="${xxx} html"
        manual_html=manual
fi
if test "x$XMLTOMAN_FALSE" = "x#"; then
        xxx="${xxx} man"
fi
if test "x$XMLTOPDF_FALSE" = "x#"; then
        xxx="${xxx} pdf"
        manual_pdf=gphoto2.pdf
fi
if test "x$XMLTOPS_FALSE" = "x#"; then 
        xxx="${xxx} ps"
        manual_pdf=gphoto2.ps
fi
AC_SUBST(manual_html)
AC_SUBST(manual_pdf)
AC_SUBST(manual_ps)

if test "x$xxx" != "x"
then
        if $have_fig2dev; then
                fig_out=""
        else
                fig_out="out"
        fi
        manual_msg="in (${xxx} ) format with${fig_out} figures"
        AC_MSG_RESULT([support for {${xxx} } found])
fi

])dnl
