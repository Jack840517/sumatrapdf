package main

import (
	"fmt"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/kjk/minio"
)

// we delete old daily and pre-release builds. This defines how many most recent
// builds to retain
const nBuildsToRetainPreRel = 5

const (
	// TODO: only remains because we want to update the version
	// so that people eventually upgrade to pre-release
	buildTypeDaily  = "daily"
	buildTypePreRel = "prerel"
	buildTypeRel    = "rel"
)

var (
	rel32Dir = filepath.Join("out", "rel32")
	rel64Dir = filepath.Join("out", "rel64")
)

func getRemotePaths(buildType string) []string {
	if buildType == buildTypePreRel {
		return []string{
			"software/sumatrapdf/sumatralatest.js",
			"software/sumatrapdf/sumpdf-prerelease-latest.txt",
			"software/sumatrapdf/sumpdf-prerelease-update.txt",
		}
	}

	if buildType == buildTypeDaily {
		return []string{
			"software/sumatrapdf/sumadaily.js",
			"software/sumatrapdf/sumpdf-daily-latest.txt",
			"software/sumatrapdf/sumpdf-daily-update.txt",
		}
	}

	if buildType == buildTypeRel {
		return []string{
			"software/sumatrapdf/sumarellatest.js",
			"software/sumatrapdf/release-latest.txt",
			"software/sumatrapdf/release-update.txt",
		}
	}

	panicIf(true, "Unkonwn buildType='%s'", buildType)
	return nil
}

func isValidBuildType(buildType string) bool {
	switch buildType {
	case buildTypeDaily, buildTypePreRel, buildTypeRel:
		return true
	}
	return false
}

// this returns version to be used in uploaded file names
func getVerForBuildType(buildType string) string {
	switch buildType {
	case buildTypePreRel, buildTypeDaily:
		// this is linear build number like "12223"
		return getPreReleaseVer()
	case buildTypeRel:
		// this is program version like "3.2"
		return sumatraVersion
	}
	panicIf(true, "invalid buildType '%s'", buildType)
	return ""
}

func getRemoteDir(buildType string) string {
	panicIf(!isValidBuildType(buildType), "invalid build type: '%s'", buildType)
	ver := getVerForBuildType(buildType)
	return "software/sumatrapdf/" + buildType + "/" + ver + "/"
}

type DownloadUrls struct {
	installer64   string
	portableExe64 string
	portableZip64 string

	installer32   string
	portableExe32 string
	portableZip32 string
}

func getDownloadUrlsForPrefix(prefix string, buildType string, ver string) *DownloadUrls {
	// zip is like .exe but can be half the size due to compression
	res := &DownloadUrls{
		installer64:   prefix + "SumatraPDF-${ver}-64-install.exe",
		portableExe64: prefix + "SumatraPDF-${ver}-64.exe",
		portableZip64: prefix + "SumatraPDF-${ver}-64.zip",
		installer32:   prefix + "SumatraPDF-${ver}-install.exe",
		portableExe32: prefix + "SumatraPDF-${ver}.exe",
		portableZip32: prefix + "SumatraPDF-${ver}.zip",
	}
	if buildType == buildTypePreRel {
		// for pre-release, ${ver} is encoded prefix
		res = &DownloadUrls{
			installer64:   prefix + "SumatraPDF-prerel-64-install.exe",
			portableExe64: prefix + "SumatraPDF-prerel-64.exe",
			portableZip64: prefix + "SumatraPDF-prerel-64.zip",
			installer32:   prefix + "SumatraPDF-prerel-install.exe",
			portableExe32: prefix + "SumatraPDF-prerel.exe",
			portableZip32: prefix + "SumatraPDF-prerel.zip",
		}
	}
	rplc := func(s *string) {
		*s = strings.Replace(*s, "${ver}", ver, -1)
		//*s = strings.Replace(*s, "${buildType}", buildType, -1)
	}
	rplc(&res.installer64)
	rplc(&res.portableExe64)
	rplc(&res.portableZip64)
	rplc(&res.installer32)
	rplc(&res.portableExe32)
	rplc(&res.portableZip32)
	return res
}

func genUpdateTxt(urls *DownloadUrls, ver string) string {
	s := `[SumatraPDF]
Latest: ${ver}
Installer64: ${inst64}
Installer32: ${inst32}
PortableExe64: ${exe64}
PortableExe32: ${exe32}
PortableZip64: ${zip64}
PortableZip32: ${zip32}
`
	rplc := func(old, new string) {
		s = strings.Replace(s, old, new, -1)
	}
	rplc("${ver}", ver)
	rplc("${inst64}", urls.installer64)
	rplc("${inst32}", urls.installer32)
	rplc("${exe64}", urls.portableExe64)
	rplc("${exe32}", urls.portableExe32)
	rplc("${zip64}", urls.portableZip64)
	rplc("${zip32}", urls.portableZip32)
	return s
}

func testGenUpdateTxt() {
	ver := "14276"
	urls := getDownloadUrlsViaWebsite(buildTypePreRel, ver)
	s := genUpdateTxt(urls, ver)
	fmt.Printf("testGenUpdateTxt:\n%s\n", s)
	os.Exit(0)
}

func getDownloadUrlsViaWebsite(buildType string, ver string) *DownloadUrls {
	prefix := "https://www.sumatrapdfreader.org/dl/" + buildType + "/" + ver + "/"
	return getDownloadUrlsForPrefix(prefix, buildType, ver)
}

func getDownloadUrlsDirectS3(mc *minio.Client, buildType string, ver string) *DownloadUrls {
	prefix := mc.URLBase()
	prefix += getRemoteDir(buildType)
	return getDownloadUrlsForPrefix(prefix, buildType, ver)
}

// sumatrapdf/sumatralatest.js
func createSumatraLatestJs(mc *minio.Client, buildType string) string {
	var appName string
	switch buildType {
	case buildTypeDaily, buildTypePreRel:
		appName = "SumatraPDF-prerel"
	case buildTypeRel:
		appName = "SumatraPDF"
	default:
		panicIf(true, "invalid buildType '%s'", buildType)
	}

	currDate := time.Now().Format("2006-01-02")
	ver := getVerForBuildType(buildType)

	// old version pointing directly to s3 storage
	//host := strings.TrimSuffix(mc.URLBase(), "/")
	//host + "software/sumatrapdf/" + buildType

	// new version that redirects via www.sumatrapdfreader.org/dl/
	host := "https://www.sumatrapdfreader.org/dl/prerel/" + ver
	if buildType == buildTypeRel {
		host = "https://www.sumatrapdfreader.org/dl/rel/" + ver
	}

	// TODO: use
	// urls := getDownloadUrls(storage, buildType, ver)

	tmplText := `
var sumLatestVer = {{.Ver}};
var sumCommitSha1 = "{{ .Sha1 }}";
var sumBuiltOn = "{{.CurrDate}}";
var sumLatestName = "{{.Prefix}}.exe";

var sumLatestExe         = "{{.Host}}/{{.Prefix}}.exe";
var sumLatestExeZip      = "{{.Host}}/{{.Prefix}}.zip";
var sumLatestPdb         = "{{.Host}}/{{.Prefix}}.pdb.zip";
var sumLatestInstaller   = "{{.Host}}/{{.Prefix}}-install.exe";

var sumLatestExe64       = "{{.Host}}/{{.Prefix}}-64.exe";
var sumLatestExeZip64    = "{{.Host}}/{{.Prefix}}-64.zip";
var sumLatestPdb64       = "{{.Host}}/{{.Prefix}}-64.pdb.zip";
var sumLatestInstaller64 = "{{.Host}}/{{.Prefix}}-64-install.exe";
`
	sha1 := getGitSha1()
	d := map[string]interface{}{
		"Host":     host,
		"Ver":      ver,
		"Sha1":     sha1,
		"CurrDate": currDate,
		"Prefix":   appName + "-" + ver,
	}
	// for prerel, version is in path, not in name
	if buildType == buildTypePreRel {
		d["Prefix"] = appName
	}
	return execTextTemplate(tmplText, d)
}

func getVersionFilesForLatestInfo(mc *minio.Client, buildType string) [][]string {
	panicIf(buildType == buildTypeRel)
	remotePaths := getRemotePaths(buildType)
	var res [][]string

	{
		// *latest.js : for the website
		s := createSumatraLatestJs(mc, buildType)
		res = append(res, []string{remotePaths[0], s})
	}

	ver := getVerForBuildType(buildType)
	{
		// *-latest.txt : for older build
		res = append(res, []string{remotePaths[1], ver})
	}

	{
		// *-update.txt : for current builds
		urls := getDownloadUrlsViaWebsite(buildType, ver)
		if false {
			urls = getDownloadUrlsDirectS3(mc, buildType, ver)
		}
		s := genUpdateTxt(urls, ver)
		res = append(res, []string{remotePaths[2], s})
	}

	return res
}

// we shouldn't re-upload files. We upload manifest-${ver}.txt last, so we
// consider a pre-release build already present in s3 if manifest file exists
func minioVerifyBuildNotInStorageMust(mc *minio.Client, buildType string) {
	dirRemote := getRemoteDir(buildType)
	ver := getVerForBuildType(buildType)
	fname := fmt.Sprintf("SumatraPDF-prerelease-%s-manifest.txt", ver)
	remotePath := path.Join(dirRemote, fname)
	exists := mc.Exists(remotePath)
	panicIf(exists, "build of type '%s' for ver '%s' already exists in s3 because file '%s' exists\n", buildType, ver, remotePath)
}

// https://kjkpubsf.sfo2.digitaloceanspaces.com/software/sumatrapdf/prerel/1024/SumatraPDF-prerelease-install.exe etc.
func minioUploadBuildMust(mc *minio.Client, buildType string) {
	timeStart := time.Now()
	defer func() {
		logf(ctx(), "Uploaded build '%s' to %s in %s\n", buildType, mc.URLBase(), time.Since(timeStart))
	}()

	dirRemote := getRemoteDir(buildType)
	getFinalDirForBuildType := func() string {
		var dir string
		switch buildType {
		case buildTypeRel:
			dir = "final-rel"
		case buildTypePreRel:
			dir = "final-prerel"
		default:
			panicIf(true, "invalid buildType '%s'", buildType)
		}
		return filepath.Join("out", dir)
	}

	dirLocal := getFinalDirForBuildType()
	//verifyBuildNotInSpaces(c, buildType)

	err := mc.UploadDir(dirRemote, dirLocal, true)
	must(err)

	// for release build we don't upload files with version info
	if buildType == buildTypeRel {
		return
	}

	uploadBuildUpdateInfoMust := func(buildType string) {
		files := getVersionFilesForLatestInfo(mc, buildType)
		for _, f := range files {
			remotePath := f[0]
			_, err := mc.UploadData(remotePath, []byte(f[1]), true)
			must(err)
			logf(ctx(), "Uploaded `%s%s'\n", mc.URLBase(), remotePath)
		}
	}

	uploadBuildUpdateInfoMust(buildType)
}

type filesByVer struct {
	ver   int
	files []string
}

func groupFilesByVersion(files []string) []*filesByVer {
	// "software/sumatrapdf/prerel/14028/SumatraPDF-prerel-64.pdb.zip"
	// =>
	// 14028
	extractVersionFromName := func(s string) int {
		parts := strings.Split(s, "/")
		verStr := parts[3]
		ver, err := strconv.Atoi(verStr)
		panicIf(err != nil, "extractVersionFromName: '%s', err='%s'\n", s, err)
		return ver
	}

	m := map[int]*filesByVer{}
	for _, f := range files {
		ver := extractVersionFromName(f)
		i := m[ver]
		if i == nil {
			i = &filesByVer{
				ver: ver,
			}
			m[ver] = i
		}
		i.files = append(i.files, f)
	}
	res := []*filesByVer{}
	for _, v := range m {
		res = append(res, v)
	}
	sort.Slice(res, func(i, j int) bool {
		return res[i].ver > res[j].ver
	})
	return res
}

func minioDeleteOldBuildsPrefix(mc *minio.Client, buildType string) {
	panicIf(buildType == buildTypeRel, "can't delete release builds")

	nBuildsToRetain := nBuildsToRetainPreRel
	remoteDir := "software/sumatrapdf/prerel/"
	objectsCh := mc.ListObjects(remoteDir)
	var keys []string
	for f := range objectsCh {
		keys = append(keys, f.Key)
		//logf(ctx(), "  %s\n", f.Key)
	}

	uri := mc.URLForPath(remoteDir)
	logf(ctx(), "%d files under '%s'\n", len(keys), uri)
	byVer := groupFilesByVersion(keys)
	for i, v := range byVer {
		deleting := (i >= nBuildsToRetain)
		if deleting {
			logf(ctx(), "deleting %d\n", v.ver)
			if true {
				for _, key := range v.files {
					err := mc.Remove(key)
					must(err)
					logf(ctx(), "  deleted %s\n", key)
				}
			}
		} else {
			logf(ctx(), "not deleting %d\n", v.ver)
		}
	}
}

func newMinioSpacesClient() *minio.Client {
	config := &minio.Config{
		Bucket:   "kjkpubsf",
		Endpoint: "sfo2.digitaloceanspaces.com",
		Access:   os.Getenv("SPACES_KEY"),
		Secret:   os.Getenv("SPACES_SECRET"),
	}
	mc, err := minio.New(config)
	must(err)
	return mc
}

func newMinioS3Client() *minio.Client {
	config := &minio.Config{
		Bucket:   "kjkpub",
		Endpoint: "s3.amazonaws.com",
		Access:   os.Getenv("AWS_ACCESS"),
		Secret:   os.Getenv("AWS_SECRET"),
	}
	mc, err := minio.New(config)
	must(err)
	return mc
}

func newMinioBackblazeClient() *minio.Client {
	config := &minio.Config{
		Bucket:   "kjk-files",
		Endpoint: "s3.us-west-001.backblazeb2.com",
		Access:   os.Getenv("BB_ACCESS"),
		Secret:   os.Getenv("BB_SECRET"),
	}
	mc, err := minio.New(config)
	must(err)
	return mc
}

func uploadToStorage(opts *BuildOptions, buildType string) {
	if !opts.upload {
		logf(ctx(), "Skipping uploadToStorage() because opts.upload = false\n")
		return
	}

	timeStart := time.Now()
	defer func() {
		logf(ctx(), "uploadToStorage of '%s' finished in %s\n", buildType, time.Since(timeStart))
	}()
	var wg sync.WaitGroup

	wg.Add(1)
	go func() {
		mc := newMinioBackblazeClient()
		minioUploadBuildMust(mc, buildType)
		minioDeleteOldBuildsPrefix(mc, buildTypePreRel)
		wg.Done()
	}()

	wg.Add(1)
	go func() {
		mc := newMinioS3Client()
		minioUploadBuildMust(mc, buildType)
		minioDeleteOldBuildsPrefix(mc, buildTypePreRel)
		wg.Done()
	}()

	wg.Add(1)
	go func() {
		mc := newMinioSpacesClient()
		minioUploadBuildMust(mc, buildType)
		minioDeleteOldBuildsPrefix(mc, buildTypePreRel)
		wg.Done()
	}()

	wg.Wait()
}

/*
change
software/sumatrapdf/rel/SumatraPDF-${ver}... =>
software/sumatrapdf/rel/${ver}/SumatraPDF...
keeps old versions to not break download links
*/
/*
func oneTimeRelCopy() {
	doCopy := func(mc *minio.Client) {
		chFiles := mc.ListObjects("software/sumatrapdf/rel/SumatraPDF-")
		for oi := range chFiles {
			parts := strings.Split(oi.Key, "/")
			name := parts[len(parts)-1]
			if name == "SumatraPDF-settings.txt" {
				fmt.Printf("skipping %s\n", oi.Key)
				continue
			}
			parts = strings.SplitN(oi.Key, "-", 3)
			var ver string
			if len(parts) == 3 {
				// software/sumatrapdf/rel/SumatraPDF-3.2-64.exe
				// =>
				// ["software/sumatrapdf/rel/SumatraPDF", "3.2", "64.exe"]
				ver = parts[1]
			} else {
				// software/sumatrapdf/rel/SumatraPDF-3.2.exe
				// =>
				// ["software/sumatrapdf/rel/SumatraPDF", "3.2.exe"]
				ver = parts[1]
				for _, suff := range []string{".exe", ".pdb.zip", ".pdb.lzsa", ".zip", ".pdb"} {
					ver = strings.Replace(ver, suff, "", -1)
				}
			}
			newKey := "software/sumatrapdf/rel/" + ver + "/" + name
			if mc.Exists(newKey) {
				fmt.Printf("Skipping copying %s because %s already exists\n", oi.Key, newKey)
				continue
			}
			_, err := mc.Copy(oi.Key, newKey)
			panicIfErr(err)
			fmt.Printf("copied %s => %s\n", oi.Key, newKey)
		}
	}
	if false {
		mc := newMinioSpacesClient()
		doCopy(mc)
	}
	if true {
		mc := newMinioBackblazeClient()
		doCopy(mc)
	}
	if true {
		mc := newMinioS3Client()
		doCopy(mc)
	}
}
*/
