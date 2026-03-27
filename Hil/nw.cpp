#include "../include/someshitdll.h"
#include <GL/glew.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <iostream>
#include <istream>
#include <ostream>

int main(int argc, const char *argv[]){
    if (!glfwInit()){
        std :: cerr << "Error initializing glfw" << std :: endl;
        return -1;
    }

    GLFWwindow* window = glfwCreateWindow(800, 500, "Name", nullptr, nullptr);

    if (!window){
        std :: cerr << "I dont know how this error will ever occur" << std :: endl;
        return -1;
    }

    int width;
    int height;
    int fbW;
    int fbH;

    HWND hwnd = glfwGetWin32Window(window);

    SomeshitDllConfig ShitConfig{};

    ShitConfig.enable_acrylic = 1;
    ShitConfig.enable_borderless = 1;
    ShitConfig.enable_quadrant_snap = 1;

    glfwMakeContextCurrent(window);

    SomeshitDllAttach(hwnd, &ShitConfig);

    while (!glfwWindowShouldClose(window)){
        glfwPollEvents();
        
        glfwGetWindowSize(window, &width, &height);
        glfwGetFramebufferSize(window, &fbW, &fbH);

        
        
        glClearColor(0.3f,0.5f,0.7f, 0.4f);
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(window);
        glfwPollEvents();
        
        glViewport(800, 500, fbW, fbH);
    }

    return 0;
}