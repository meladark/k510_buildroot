#!/bin/sh
# Builds the model zoo on the build server:
#   onnx/<id>.onnx      exported heads (k510-model-export image)
#   kmodel/<id>_<q>.*   compiled + simulated (k510-model-compile image)
# Usage: build_all.sh [model:size ...]   (default: the list below)
# Work dir: /mnt/nvme0n1/k510/models (photos/, coco128/, scripts/ = this dir)
set -e
W=/mnt/nvme0n1/k510/models
# yolo11 is left out: nncase 1.9 miscompiles its attention block (see README)
MODELS=${*:-"yolov5n:320 yolov5s:320 yolov5m:320 yolov8n:320 yolov8s:320 yolov5n:640 yolov5s:640 yolov8n:640"}
U="$(id -u):$(id -g)"
cd $W
for ms in $MODELS; do
	id=${ms%:*}_${ms#*:}
	[ -f onnx/$id.onnx ] || docker run --rm -u $U -e HOME=/tmp -e YOLO_CONFIG_DIR=/tmp -v $W:/w -w /w/work \
		k510-model-export python /w/scripts/export.py --name ${ms%:*} --size ${ms#*:} --out /w/onnx
done
# compile three at a time; each job logs to kmodel/<id>.log
mkdir -p kmodel
for ms in $MODELS; do
	id=${ms%:*}_${ms#*:}
	for q in bf16 uint8; do
		[ -f kmodel/${id}_$q.kmodel ] && continue
		echo "$id $q"
	done
done | xargs -P 3 -L 1 sh -c 'docker run --rm -u '"$U"' -e HOME=/tmp -v '"$W"':/w -w /w/scripts k510-model-compile \
	python3 compile.py --onnx /w/onnx/$0.onnx --quant $1 --out /w/kmodel \
	--calib "/w/photos/*/raw/*.jpg" "/w/coco128/images/train2017/*.jpg" --test $(head -2 '"$W"'/test_images.txt) \
	> '"$W"'/kmodel/$0_$1.log 2>&1; echo "$0 $1 rc=$?"; grep -E "MB,|sim" '"$W"'/kmodel/$0_$1.log'
