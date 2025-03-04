// Copyright 2020 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

package utils

import (
	"bufio"
	"crypto/md5"
	"encoding/hex"
	"fmt"
	"io"
	"strconv"
	"strings"

	"github.com/spf13/afero"
)

func ReadFileLines(fs afero.Fs, filePath string) ([]string, error) {
	file, err := fs.Open(filePath)
	var lines []string
	if err != nil {
		return nil, err
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		lines = append(lines, scanner.Text())
	}

	if err := scanner.Err(); err != nil {
		return nil, err
	}
	return lines, nil
}

func ReadEnsureSingleLine(fs afero.Fs, path string) (string, error) {
	lines, err := ReadFileLines(fs, path)
	if err != nil {
		return "", err
	}
	if len(lines) == 0 {
		return "", fmt.Errorf("%s is empty", path)
	}
	if len(lines) > 1 {
		return "", fmt.Errorf("%s contains multiple lines", path)
	}
	return lines[0], nil
}

func ListFilesInPath(fs afero.Fs, path string) []string {
	var names []string
	file, _ := fs.Open(path)
	files, _ := file.Readdir(0)
	for _, fileInfo := range files {
		names = append(names, fileInfo.Name())
	}
	return names
}

func CopyFile(fs afero.Fs, src string, dst string) error {
	input, err := afero.ReadFile(fs, src)
	if err != nil {
		return err
	}
	err = afero.WriteFile(fs, dst, input, 0o644)
	return err
}

func WriteFileLines(fs afero.Fs, lines []string, path string) error {
	return afero.WriteFile(fs, path, []byte(strings.Join(lines, "\n")+"\n"), 0o600)
}

func WriteBytes(fs afero.Fs, bs []byte, path string) (int, error) {
	return len(bs), afero.WriteFile(fs, path, bs, 0o600)
}

func FileMd5(fs afero.Fs, filePath string) (string, error) {
	var returnMD5String string
	file, err := fs.Open(filePath)
	if err != nil {
		return returnMD5String, err
	}
	defer file.Close()
	hash := md5.New()
	if _, err := io.Copy(hash, file); err != nil {
		return returnMD5String, err
	}
	hashInBytes := hash.Sum(nil)
	returnMD5String = hex.EncodeToString(hashInBytes)
	return returnMD5String, nil
}

func BackupFile(fs afero.Fs, filePath string) (string, error) {
	md5, err := FileMd5(fs, filePath)
	if err != nil {
		return "", err
	}
	bkFilePath := fmt.Sprintf("%s.vectorized.%s.bk", filePath, md5)
	err = CopyFile(fs, filePath, bkFilePath)
	if err != nil {
		return "", fmt.Errorf("unable to create backup of %s", filePath)
	}
	return bkFilePath, nil
}

func ReadIntFromFile(fs afero.Fs, file string) (int, error) {
	content, err := ReadEnsureSingleLine(fs, file)
	if err != nil {
		return 0, err
	}
	return strconv.Atoi(strings.TrimSpace(content))
}
