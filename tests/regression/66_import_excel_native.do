* 66_import_excel_native — native xlsx reader (src/xlsx.c): no ssconvert,
* no gnumeric, identical on native and WASM/browser builds.  zip +
* inflate + sheet XML; shared strings, quotes and commas inside cells,
* sheet() selection by name, loud error for a missing sheet.  This is
* what makes `import excel` work in the browser at all.
import excel tests/fixtures/mini.xlsx, firstrow clear
describe
list
import excel tests/fixtures/mini.xlsx, sheet("Other") firstrow clear
list
capture import excel tests/fixtures/mini.xlsx, sheet("NoSuch") clear
display _rc
