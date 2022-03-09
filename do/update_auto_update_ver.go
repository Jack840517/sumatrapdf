package main

import (
	"fmt"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/kjk/minio"
)

// Format of auto-update file:
/*
[SumatraPDF]
Latest 3.1.2
*/

/*
for 3.1.2 and earlier, we upload:
https://kjkpub.s3.amazonaws.com/sumatrapdf/sumpdf-update.txt
https://kjkpub.s3.amazonaws.com/sumatrapdf/sumpdf-latest.txt

for 3.2 and later, we use
https://www.sumatrapdfreader.org/update-check-rel.txt

This script uploads to s3 and updates website/update-check-rel.txt,
which must be then deployed.
*/

// ver should be in format:
// 3
// 3.1
// 3.1.2
func validateVer(ver string) {
	parts := strings.Split(ver, ".")
	panicIf(len(parts) > 3)
	for i, p := range parts {
		n, err := strconv.Atoi(p)
		must(err)
		panicIf(n < 0 || n > 19)
		panicIf(i == 0 && n != 3, "major version must be 3")
	}
}

func updateAutoUpdateVer(ver string) {
	validateVer(ver)
	// TODO: verify it's bigger than the current version
	// TODO: add download links
	s := fmt.Sprintf(`[SumatraPDF]
Latest %s
`, ver)
	fmt.Printf("Content of update file:\n%s\n\n", s)
	d := []byte(s)

	uploadInfo := func(mc *minio.Client) {
		{
			remotePath := "sumatrapdf/sumpdf-update.txt"
			_, err := mc.UploadData(remotePath, d, true)
			must(err)
		}
		{
			remotePath := "sumatrapdf/sumpdf-latest.txt"
			_, err := mc.UploadData(remotePath, d, true)
			must(err)
		}
	}

	uploadInfo(newMinioS3Client())
	uploadInfo(newMinioBackblazeClient())
	uploadInfo(newMinioSpacesClient())

	path := filepath.Join("website", "update-check-rel.txt")
	writeFileMust(path, []byte(s))

	fmt.Printf("Don't forget to checkin file '%s' and deploy website\n", path)
}
