//********************************************************//
// CUDA SIFT extractor by Marten Björkman aka Celebrandil //
//              celle @ csc.kth.se                       //
//********************************************************//

#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <tbb/parallel_for.h>
#include <tbb/task_scheduler_init.h>
#include <thread>

#include "cudasift/cudaImage.h"
#include "cudasift/cudaSift.h"

int ImproveHomography(SiftData &data, float *homography, int numLoops, float minScore, float maxAmbiguity, float thresh);
void PrintMatchData(SiftData &siftData1, SiftData &siftData2, CudaImage &img);
void MatchAll(SiftData &siftData1, SiftData &siftData2, float *homography);

double ScaleUp(CudaImage &res, CudaImage &src);

///////////////////////////////////////////////////////////////////////////////
// Main program
///////////////////////////////////////////////////////////////////////////////
int main(int argc, char **argv)
{
  int devNum = 0, imgSet = 0;
  if (argc>1)
    devNum = std::atoi(argv[1]);
  if (argc>2)
    imgSet = std::atoi(argv[2]);

  // Read images using OpenCV
  cv::Mat limg, rimg;
  if (imgSet) {
    cv::imread("data/left.pgm", 0).convertTo(limg, CV_32FC1);
    cv::imread("data/righ.pgm", 0).convertTo(rimg, CV_32FC1);
  } else {
    cv::imread("data/img1.png", 0).convertTo(limg, CV_32FC1);
    cv::imread("data/img2.png", 0).convertTo(rimg, CV_32FC1);
  }
  //cv::flip(limg, rimg, -1);
  int w = limg.cols;
  int h = limg.rows;
  std::cout << "Image size = (" << w << "," << h << ")" << std::endl;

  constexpr int num_features = 0x8000;
  float initBlur = 1.0f;
  float thresh = (imgSet ? 4.5f : 3.0f);

  // Initial Cuda images and download images to device
  std::cout << "Initializing data..." << std::endl;
  InitCuda(num_features, 5, initBlur, devNum);
  {
    CudaImage img1, img2;
    cudaStream_t stream1, stream2;
    cudaStreamCreate(&stream1);
    cudaStreamCreate(&stream2);
    img1.Allocate(w, h, iAlignUp(w, 128), false, NULL, (float *)limg.data,
                  stream1);
    img2.Allocate(w, h, iAlignUp(w, 128), false, NULL, (float *)rimg.data,
                  stream2);
    img1.Download();
    img2.Download();

    DescriptorNormalizerData data;
    data.n_steps = 5;
    data.n_data = 1;
    int steps[] = {1, 4, 1, 3, 0};
    float dataf[] = {0.2f};
    data.normalizer_steps = steps;
    data.data = dataf;
    DeviceDescriptorNormalizerData d_normalizer(data);

    // Extract Sift features from images
    DeviceSiftData siftData1(num_features), siftData2(num_features);

    // A bit of benchmarking
    // for (float thresh1=1.00f;thresh1<=4.01f;thresh1+=0.50f) {
    TempMemory memoryTmp1(w, h, 5, false);
    TempMemory memoryTmp2(w, h, 5, false);
    ExtractSift(siftData1, d_normalizer, img1, 5, thresh, 0.0f, false, memoryTmp1, stream1);
    ExtractSift(siftData2, d_normalizer, img2, 5, thresh, 0.0f, false, memoryTmp2, stream2);

    constexpr int iterations = 1000;

    auto bench_start = std::chrono::high_resolution_clock::now();
    std::thread thread1([&]() {
      for (int i = 0; i < iterations; i++) {
        ExtractSift(siftData1, d_normalizer, img1, 5, thresh, 0.0f, false,
                    memoryTmp1, stream1);
      }
    });
    std::thread thread2([&]() {
      for (int i = 0; i < iterations; i++) {
        ExtractSift(siftData2, d_normalizer, img2, 5, thresh, 0.0f, false,
                    memoryTmp2, stream2);
      }
    });
    thread1.join();
    thread2.join();
    auto bench_end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> bench_ms =
        bench_end - bench_start;
    std::cout << "Simple 2 thread benchmark (excluding copy): " << bench_ms.count() << " ms, "
              << iterations*2. * 1000. / bench_ms.count() << " fps\n";

    // Match Sift features and find a homography
    for (int i=0;i<1;i++)
      MatchSiftData(siftData1, siftData2, stream1);
    float homography[9];
    int numMatches;
    FindHomography(siftData1, homography, &numMatches, 10000, 0.00f, 0.95f,
                   5.0, stream1);
    SiftData hostData1(num_features);
    siftData1.downloadFeatures(hostData1, stream1);
    cudaStreamSynchronize(stream1);
    int numFit = ImproveHomography(hostData1, homography, 5, 0.00f, 0.95f, 3.0);

    std::cout << "Number of original features: " <<  siftData1.numPts << " " << siftData2.numPts << std::endl;
    std::cout << "Number of matching features: " << numFit << " " << numMatches << " " << 100.0f*numFit/std::min(siftData1.numPts, siftData2.numPts) << "% " << initBlur << " " << thresh << std::endl;
    //}

    SiftData hostData2(num_features);
    siftData2.downloadFeatures(hostData2);
    // Print out and store summary data
    PrintMatchData(hostData1, hostData2, img1);
    cv::imwrite("data/limg_pts.pgm", limg);

    //MatchAll(siftData1, siftData2, homography);

    // Free Sift data from device
    cudaStreamDestroy(stream1);
    cudaStreamDestroy(stream2);
  }

  std::cout << "Multithreaded benchmark\n";

  DescriptorNormalizerData norm_data;
  norm_data.n_steps = 5;
  norm_data.n_data = 1;
  int steps[] = {1, 4, 1, 3, 0};
  float dataf[] = {0.2f};
  norm_data.normalizer_steps = steps;
  norm_data.data = dataf;
  DeviceDescriptorNormalizerData d_normalizer(norm_data);

  for (int i = 1; i <= 16; ++i) {
    std::vector<TempMemory> memoryTmp;
    std::vector<cudaStream_t> streams;
    std::vector<CudaImage> imgs;
    std::vector<DeviceSiftData> siftData;
    for (int j = 0; j < i; ++j) {
      memoryTmp.emplace_back(w, h, 5, false);
      cudaStream_t stream;
      cudaStreamCreate(&stream);
      streams.push_back(stream);
      CudaImage img;
      img.Allocate(w, h, iAlignUp(w, 128), false, nullptr, (float*)limg.data, stream);
      imgs.push_back(std::move(img));
      DeviceSiftData data(num_features);
      siftData.push_back(std::move(data));
    }

    tbb::task_scheduler_init scheduler{i};
    std::atomic<int> ctr{0};

    constexpr int iterations = 1000;

    auto bench_start = std::chrono::high_resolution_clock::now();
    tbb::parallel_for(0, iterations, [&](const auto &) {
      int tid = ctr++ % i;
      imgs[tid].Download();
      ExtractSift(siftData[tid], d_normalizer, imgs[tid], 5, thresh, 0.0f, false,
                  memoryTmp[tid], streams[tid]);
    });
    for (int j = 0; j < i; ++j) {
      cudaStreamSynchronize(streams[j]);
    }
    auto bench_end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> bench_ms =
        bench_end - bench_start;
    std::cout << i << " threads: " << bench_ms.count() << " ms, "
              << iterations * 1000. / bench_ms.count() << " fps\n";
    for (int j = 0; j < i; ++j) {
      cudaStreamDestroy(streams[j]);
    }
  }
}

void MatchAll(SiftData &siftData1, SiftData &siftData2, float *homography)
{
#ifdef MANAGEDMEM
  SiftPoint *sift1 = siftData1.m_data;
  SiftPoint *sift2 = siftData2.m_data;
#else
  SiftPoint *sift1 = siftData1.h_data;
  SiftPoint *sift2 = siftData2.h_data;
#endif
  int numPts1 = siftData1.numPts;
  int numPts2 = siftData2.numPts;
  int numFound = 0;
#if 1
  homography[0] = homography[4] = -1.0f;
  homography[1] = homography[3] = homography[6] = homography[7] = 0.0f;
  homography[2] = 1279.0f;
  homography[5] = 959.0f;
#endif
  for (int i=0;i<numPts1;i++) {
    float *data1 = sift1[i].data;
    std::cout << i << ":" << sift1[i].scale << ":" << (int)sift1[i].orientation << " " << sift1[i].xpos << " " << sift1[i].ypos << std::endl;
    bool found = false;
    for (int j=0;j<numPts2;j++) {
      float *data2 = sift2[j].data;
      float sum = 0.0f;
      for (int k=0;k<128;k++)
	sum += data1[k]*data2[k];
      float den = homography[6]*sift1[i].xpos + homography[7]*sift1[i].ypos + homography[8];
      float dx = (homography[0]*sift1[i].xpos + homography[1]*sift1[i].ypos + homography[2]) / den - sift2[j].xpos;
      float dy = (homography[3]*sift1[i].xpos + homography[4]*sift1[i].ypos + homography[5]) / den - sift2[j].ypos;
      float err = dx*dx + dy*dy;
      if (err<100.0f) // 100.0
	found = true;
      if (err<100.0f || j==sift1[i].match) { // 100.0
	if (j==sift1[i].match && err<100.0f)
	  std::cout << " *";
	else if (j==sift1[i].match)
	  std::cout << " -";
	else if (err<100.0f)
	  std::cout << " +";
	else
	  std::cout << "  ";
	std::cout << j << ":" << sum << ":" << (int)sqrt(err) << ":" << sift2[j].scale << ":" << (int)sift2[j].orientation << " " << sift2[j].xpos << " " << sift2[j].ypos << " " << (int)dx << " " << (int)dy << std::endl;
      }
    }
    std::cout << std::endl;
    if (found)
      numFound++;
  }
  std::cout << "Number of finds: " << numFound << " / " << numPts1 << std::endl;
  std::cout << homography[0] << " " << homography[1] << " " << homography[2] << std::endl;//%%%
  std::cout << homography[3] << " " << homography[4] << " " << homography[5] << std::endl;//%%%
  std::cout << homography[6] << " " << homography[7] << " " << homography[8] << std::endl;//%%%
}

void PrintMatchData(SiftData &siftData1, SiftData &siftData2, CudaImage &img)
{
  int numPts = siftData1.numPts;
#ifdef MANAGEDMEM
  SiftPoint *sift1 = siftData1.m_data;
  SiftPoint *sift2 = siftData2.m_data;
#else
  SiftPoint *sift1 = siftData1.h_data;
  SiftPoint *sift2 = siftData2.h_data;
#endif
  float *h_img = img.h_data;
  int w = img.width;
  int h = img.height;
  std::cout << std::setprecision(3);
  for (int j=0;j<numPts;j++) {
    int k = sift1[j].match;
    if (sift1[j].match_error<5) {
      float dx = sift2[k].xpos - sift1[j].xpos;
      float dy = sift2[k].ypos - sift1[j].ypos;
#if 0
      if (false && sift1[j].xpos>550 && sift1[j].xpos<600) {
	std::cout << "pos1=(" << (int)sift1[j].xpos << "," << (int)sift1[j].ypos << ") ";
	std::cout << j << ": " << "score=" << sift1[j].score << "  ambiguity=" << sift1[j].ambiguity << "  match=" << k << "  ";
	std::cout << "scale=" << sift1[j].scale << "  ";
	std::cout << "error=" << (int)sift1[j].match_error << "  ";
	std::cout << "orient=" << (int)sift1[j].orientation << "," << (int)sift2[k].orientation << "  ";
	std::cout << " delta=(" << (int)dx << "," << (int)dy << ")" << std::endl;
      }
#endif
#if 1
      int len = (int)(fabs(dx)>fabs(dy) ? fabs(dx) : fabs(dy));
      for (int l=0;l<len;l++) {
	int x = (int)(sift1[j].xpos + dx*l/len);
	int y = (int)(sift1[j].ypos + dy*l/len);
	h_img[y*w+x] = 255.0f;
      }
#endif
    }
    int x = (int)(sift1[j].xpos+0.5);
    int y = (int)(sift1[j].ypos+0.5);
    int s = std::min(x, std::min(y, std::min(w-x-2, std::min(h-y-2, (int)(1.41*sift1[j].scale)))));
    int p = y*w + x;
    p += (w+1);
    for (int k=0;k<s;k++)
      h_img[p-k] = h_img[p+k] = h_img[p-k*w] = h_img[p+k*w] = 0.0f;
    p -= (w+1);
    for (int k=0;k<s;k++)
      h_img[p-k] = h_img[p+k] = h_img[p-k*w] =h_img[p+k*w] = 255.0f;
  }
  std::cout << std::setprecision(6);
}


