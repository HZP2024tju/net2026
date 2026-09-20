#!/bin/bash

rm -rf  .git/
git init
git branch -m master week2
git remote add nerd  https://github.com/HZP2024tju/net2026.git
git add .
git commit -m "damn"
git push --set-upstream nerd week2 -f