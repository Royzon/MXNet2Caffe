/**
* Copyright (C) DeepGlint, Inc - All Rights Reserved
* Unauthorized copying of this file, via any medium is strictly prohibited
* Proprietary and confidential
*
* Mxnet2Caffe: main Function
*
* Written by Devymex <yumengwang@deepglint.com>, Jan. 2019
*/

#include <functional>
#include <iostream>
#include <fstream>
#include <map>
#include <google/protobuf/text_format.h>
#include <glog/logging.h>

#include "common.hpp"
#include "json_helper.hpp"
#include "mxnet_parser.hpp"
#include "converter.hpp"

namespace proto = google::protobuf;
using InputInfo = std::pair<std::string, Shape>;

struct ProgramOptions {
	std::string strMxnetJson;
	std::string strMxnetParams;
	std::string strCaffeProto;
	std::string strCaffeModel;
	std::vector<InputInfo> inputInfos;
};

StringPair SplitString(std::string str, size_t nPos) {
	if (nPos == 0) {
		return std::make_pair(std::string(), std::move(str));
	} else if (nPos >= str.size()) {
		return std::make_pair(std::move(str), std::string());
	}
	return std::make_pair(str.substr(0, nPos),
			str.substr(nPos, str.size() - nPos));
}

bool ParseArgument(int nArgCnt, char **ppArgs, ProgramOptions &po) {
	if (nArgCnt < 2) {
		return false;
	}
	std::string strConfFn = ppArgs[1];
	std::string strWorkPath;
	auto pathAndName = SplitString(strConfFn,
			strConfFn.find_last_of("\\/") + 1);
	if (!pathAndName.second.empty()) {
		strWorkPath = pathAndName.first;
	}

	std::ifstream configFile(strConfFn);
	CHECK(configFile.is_open());
	Json jConfig;
	configFile >> jConfig;
	configFile.close();

	po.strMxnetJson = strWorkPath + std::string(jConfig["mxnet_json"]);
	po.strMxnetParams = strWorkPath + std::string(jConfig["mxnet_params"]);
	po.strCaffeProto = strWorkPath + std::string(jConfig["caffe_prototxt"]);
	po.strCaffeModel = strWorkPath + std::string(jConfig["caffe_caffemodel"]);

	Json::iterator jInputs = jConfig.find("inputs");
	po.inputInfos = ParseArray<InputInfo>(jInputs,
			[](Json::iterator iInput) -> InputInfo {
				std::string strName = (*iInput)["name"];
				CHECK(!strName.empty());
				Json::iterator jShape = iInput->find("shape");
				CHECK(jShape != iInput->end());
				auto intShape = ParseArray<int>(jShape);
				CHECK(!intShape.empty());
				Shape shape;
				for (auto s : intShape) {
					CHECK_GT(s, 0);
					shape.emplace_back(s);
				}
				return std::make_pair(strName, shape);
			}
		);
	CHECK(!po.inputInfos.empty());
	return true;
}

std::string GenerateModelName(std::string strProtoFn) {
	auto pathAndName = SplitString(strProtoFn,
			strProtoFn.find_last_of("\\/") + 1);
	if (pathAndName.second.empty()) {
		std::swap(pathAndName.first, pathAndName.second);
	}
	strProtoFn = pathAndName.second;
	auto nameAndExt = SplitString(strProtoFn, strProtoFn.find_last_of("."));
	if (nameAndExt.first.empty()) {
		std::swap(nameAndExt.first, nameAndExt.second);
	}
	return nameAndExt.first;
}

int main(int nArgCnt, char *ppArgs[]) {
	ProgramOptions po;
	if (!ParseArgument(nArgCnt, ppArgs, po)) {
		return -1;
	}

	auto mxnetParseResult = ParseMxnetJson(po.strMxnetJson);
	auto mxnetParams = LoadMxnetParam(po.strMxnetParams);
	std::map<std::string, std::vector<std::string>> blobMapping;
	auto protoNet = MxnetNodes2CaffeNet(
			mxnetParseResult.first, mxnetParseResult.second,
			po.inputInfos, blobMapping);
	protoNet.set_name(GenerateModelName(po.strCaffeProto));

	std::string strProtoBuf;
	proto::TextFormat::PrintToString(protoNet, &strProtoBuf);
	std::ofstream protoFile(po.strCaffeProto);
	protoFile.write(strProtoBuf.data(), strProtoBuf.size());
	protoFile.close();

	//caffe::Net<float> net(protoNet);
	//auto &layers = net.layers();
	for (auto &layer : *(protoNet.mutable_layer())) {
		auto iBlobMap = blobMapping.find(layer.name());
		if (iBlobMap != blobMapping.end()) {
			auto &blobNames = iBlobMap->second;
			auto *pProtoBlobs = layer.mutable_blobs();
			if (layer.type() == "BatchNorm") {
				CHECK_EQ(blobNames.size(), 2);
				for (int i = 0; i < 3; ++i) {
					layer.add_blobs();
				}
				layer.mutable_blobs(2)->add_data(1.0f);
			}
			for (size_t i = 0; i < blobNames.size(); ++i) {
				auto iMxnetParam = std::find_if(mxnetParams.begin(),
						mxnetParams.end(), [&](const MxnetParam &param) {
							return param.strName == blobNames[i];
						}
					);
				CHECK(iMxnetParam != mxnetParams.end());
				size_t nBlobSize = iMxnetParam->data.size();

				auto *pBlob = layer.add_blobs();
				auto *pShape = pBlob->mutable_shape();
				pShape->add_dim(nBlobSize);

				pBlob->mutable_data()->Resize(nBlobSize, 0.f);
				memcpy(pBlob->mutable_data()->mutable_data(),
						iMxnetParam->data.data(),
						nBlobSize * sizeof(float));
			}
		}
	}
	std::ofstream outBinFile(po.strCaffeModel.c_str(), std::ofstream::binary);
	protoNet.SerializeToOstream(&outBinFile);
	outBinFile.close();

	return 0;
}

